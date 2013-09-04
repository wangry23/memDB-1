/*-------------------------------------------------------------------------
 *
 * utility.c
 *	  Contains functions which control the execution of the POSTGRES utility
 *	  commands.  At one time acted as an interface between the Lisp and C
 *	  systems.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/utility.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/toasting.h"
#include "commands/alter.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/collationcmds.h"
#include "commands/conversioncmds.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/discard.h"
#include "commands/explain.h"
#include "commands/extension.h"
#include "commands/lockcmds.h"
#include "commands/portalcmds.h"
#include "commands/prepare.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/seclabel.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "postmaster/bgwriter.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteRemove.h"
#include "storage/fd.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/syscache.h"
// Additional includes for CreateRStmt
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "access/htup.h"
#include "catalog/index.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "parser/parse_relation.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "utils/rel.h"
#include "utils/typcache.h"
// Additional include for DropRecStmt
#include "executor/executor.h"
#include "utils/recathon.h"

/* Hook for plugins to get control in ProcessUtility() */
ProcessUtility_hook_type ProcessUtility_hook = NULL;

/* Several functions created for the CreateRStmt. */
static void freeCellList(cell_node head);

/* The functions for creating recommendation models. */
static void itemSimilarity(CreateRStmt *recStmt, attr_node attList, int numatts, recMethod method);
static void userSimilarity(CreateRStmt *recStmt, attr_node attList, int numatts, recMethod method);
static void SVDSimilarity(CreateRStmt *recStmt, attr_node attList, int numatts);

/* Function to free a list of cell_nodes. */
static void freeCellList(cell_node head) {
	cell_node temp;
	while (head) {
		temp = head->next;
		if (head->modelname1)
			pfree(head->modelname1);
		if (head->modelname2)
			pfree(head->modelname2);
		if (head->viewname)
			pfree(head->viewname);
		pfree(head);
		head = temp;
	}
}

/*
 * Create item similarity matrices for each cell in a recommender.
 */
static void itemSimilarity(CreateRStmt *recStmt, attr_node attList, int numatts, recMethod method) {
	// For cosine similarity, we will constantly re-use the vector
	// length of each item, and that's a pain to calculate. We're
	// going to pre-calculate that information, store it in an
	// array, and refer to it when necessary.
	int *itemIDs;
	float *itemLengths;
	// For Pearson correlation.
	float *itemAvgs, *itemPearsons;
	int i, numItems;
	// Objects for querying.
	char *querystring;
	QueryDesc *queryDesc;
	PlanState *planstate;
	TupleTableSlot *slot;
	MemoryContext recathoncontext;

	// For cosine, we obtain the list of items, the number of items, and their vector lengths.
	if (method == itemCosCF)
		itemLengths = vector_lengths(recStmt->itemtable->relname,recStmt->itemkey,
				recStmt->ratingtable->relname,recStmt->ratingval,&numItems,&itemIDs);

	// For Pearson, we obtain the list of items, the number of items, their average ratings,
	// and another useful constant (I'm calling them Pearsons).
	else if (method == itemPearCF)
		pearson_info(recStmt->itemtable->relname,recStmt->itemkey,recStmt->ratingtable->relname,
			recStmt->ratingval,&numItems,&itemIDs,&itemAvgs,&itemPearsons);

	// Populate the recindex table, with all the necessary entries.
	// If the recommender is context-based, then we need to obtain
	// all of the possible value combinations, to make an entry
	// for each one. If it's context-free, we just need the one,
	// and no context information necessary.
	if (numatts > 0) {
		attr_node temp_node;
		char *attquerystring;
		char **attnames, **attvalues;

		// We'll temporarily store attribute names in this
		// structure. It's handy to have an array, especially
		// something that corresponds one-to-one with the values
		// structure.
		attnames = (char**) palloc(numatts*sizeof(char*));
		temp_node = attList;
		for (i = 0; i < numatts; i++) {
			attnames[i] = (char*) palloc(256*sizeof(char));
			sprintf(attnames[i],"%s",temp_node->colname);
			temp_node = temp_node->next;
		}

		// We'll store the attribute values in this.
		attvalues = (char**) palloc(numatts*sizeof(char*));

		// We need to select each value combination.
		attquerystring = (char*) palloc(1024*sizeof(char));
		sprintf(attquerystring,"SELECT DISTINCT ");
		for (temp_node = attList; temp_node; temp_node = temp_node->next) {
			strncat(attquerystring,temp_node->colname,strlen(temp_node->colname));
			if (temp_node->next)
				strncat(attquerystring,", ",2);
		}
		strncat(attquerystring," FROM ",6);
		strncat(attquerystring,recStmt->usertable->relname,strlen(recStmt->usertable->relname));
		strncat(attquerystring,";",2);

		// Now that we have a query formulated, we look through the
		// results and insert one entry into the recindex for each
		// result.
		queryDesc = recathon_queryStart(attquerystring, &recathoncontext);
		planstate = queryDesc->planstate;

		// Look through the results of our SELECT DISTINCT query.
		for (;;) {
			int j;
			int numRatings = 0;
			char *recindexname, *recmodelname;
			struct timeval timestamp;

			// Let's quickly fill in the name of the recIndex,
			// since it'll be an argument for a later function.
			recindexname = (char*) palloc((6+strlen(recStmt->relation->relname))*sizeof(char));
			sprintf(recindexname,"%sIndex",recStmt->relation->relname);

			slot = ExecProcNode(planstate);
			if (TupIsNull(slot)) break;

			// Get each of the needed attribute values from our tuple.
			for (j = 0; j < numatts; j++)
				attvalues[j] = getTupleString(slot,attnames[j]);

			// There's no way a "SELECT DISTINCT" clause is going
			// to turn up NULL values, so our attvalues array should
			// be full now.
			querystring = (char*) palloc(1024*sizeof(char));
			gettimeofday(&timestamp,NULL);

			// Let's name the model.
			recmodelname = (char*) palloc(256*sizeof(char));
			sprintf(recmodelname,"%sModel%ld%ld",recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec);

			// We need to create a RecModel and calculate item similarities.
			sprintf(querystring,"CREATE TABLE %s (item1 INTEGER NOT NULL, item2 INTEGER NOT NULL, similarity REAL NOT NULL);",
				recmodelname);
			// Execute the INSERT.
			recathon_utilityExecute(querystring);

			// Create a blank RecView for use with FILTERRECOMMEND
			// and JOINRECOMMEND, and populate with a blank tuple.
			sprintf(querystring,"CREATE TABLE %sView%ld%ld (%s INTEGER NOT NULL, %s INTEGER NOT NULL, PRIMARY KEY (%s, %s), recscore REAL NOT NULL);",
				recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec,
				recStmt->userkey,recStmt->itemkey,
				recStmt->userkey,recStmt->itemkey);
			recathon_utilityExecute(querystring);

			// Insert a dummy tuple into the RecView.
			sprintf(querystring,"INSERT INTO %sView%ld%ld VALUES(-1,-1,-1);",
				recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec);
			recathon_queryExecute(querystring);

			// The task of populating the similarity matrix is left to an
			// external function.
			if (method == itemCosCF)
				numRatings = updateItemCosModel(recStmt->usertable->relname,recStmt->itemtable->relname,
							recStmt->ratingtable->relname,recStmt->userkey,recStmt->itemkey,
							recStmt->ratingval,recmodelname,numatts,attnames,attvalues,
							itemIDs,itemLengths,numItems,false);
			else if (method == itemPearCF)
				numRatings = updateItemPearModel(recStmt->usertable->relname,recStmt->itemtable->relname,
							recStmt->ratingtable->relname,recStmt->userkey,recStmt->itemkey,
							recStmt->ratingval,recmodelname,numatts,attnames,attvalues,
							itemIDs,itemAvgs,itemPearsons,numItems,false);

			// Now we can insert an entry into the index table for this cell.
			sprintf(querystring,"INSERT INTO %s VALUES (default, '%s', '%sView%ld%ld', 0, %d, 0, 0.0, 0.0, localtimestamp",
				recindexname,recmodelname,recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec,numRatings);
			// We need to add the attribute values.
			for (i = 0; i < numatts; i++) {
				char addition[256];
				sprintf(addition,", '%s'",attvalues[i]);
				strncat(querystring,addition,strlen(addition));
			}
			strncat(querystring,");",2);

			// Now execute the INSERT query.
			recathon_queryExecute(querystring);
			pfree(querystring);
		}

		// Some memory cleanup.
		recathon_queryEnd(queryDesc, recathoncontext);
		for (i = 0; i < numatts; i++) {
			pfree(attnames[i]);
			pfree(attvalues[i]);
		}
		pfree(attnames);
		pfree(attvalues);

	// In case our recommender is context-free, our work is much easier.
	} else {
		int numRatings = 0;
		char *recindexname, *recmodelname;
		struct timeval timestamp;

		// Let's quickly fill in the name of the recIndex,
		// since it'll be an argument for a later function.
		recindexname = (char*) palloc((6+strlen(recStmt->relation->relname))*sizeof(char));
		sprintf(recindexname,"%sIndex",recStmt->relation->relname);

		// Inserting the one entry for the recindex.
		querystring = (char*) palloc(1024*sizeof(char));
		gettimeofday(&timestamp,NULL);

		// Let's name the model.
		recmodelname = (char*) palloc(256*sizeof(char));
		sprintf(recmodelname,"%sModel%ld%ld",recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec);

		// We need to create a RecModel and calculate item similarities.
		sprintf(querystring,"CREATE TABLE %s (item1 INTEGER NOT NULL, item2 INTEGER NOT NULL, similarity REAL NOT NULL);",
			recmodelname);
		// Execute the INSERT.
		recathon_utilityExecute(querystring);

		// Create a blank RecView for use with FILTERRECOMMEND
		// and JOINRECOMMEND, and populate with a blank tuple.
		sprintf(querystring,"CREATE TABLE %sView%ld%ld (%s INTEGER NOT NULL, %s INTEGER NOT NULL, PRIMARY KEY (%s, %s), recscore REAL NOT NULL);",
			recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec,
			recStmt->userkey,recStmt->itemkey,
			recStmt->userkey,recStmt->itemkey);
		recathon_utilityExecute(querystring);

		// Insert a dummy tuple into the RecView.
		sprintf(querystring,"INSERT INTO %sView%ld%ld VALUES(-1,-1,-1);",
			recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec);
		recathon_queryExecute(querystring);

		// The task of populating the similarity matrix is left to an
		// external function.
		if (method == itemCosCF)
			numRatings = updateItemCosModel(recStmt->usertable->relname,recStmt->itemtable->relname,
						recStmt->ratingtable->relname,recStmt->userkey,recStmt->itemkey,
						recStmt->ratingval,recmodelname,numatts,NULL,NULL,
						itemIDs,itemLengths,numItems,false);
		else if (method == itemPearCF)
			numRatings = updateItemPearModel(recStmt->usertable->relname,recStmt->itemtable->relname,
						recStmt->ratingtable->relname,recStmt->userkey,recStmt->itemkey,
						recStmt->ratingval,recmodelname,numatts,NULL,NULL,
						itemIDs,itemAvgs,itemPearsons,numItems,false);

		// Now we can insert an entry into the index table for this cell.
		sprintf(querystring,"INSERT INTO %s VALUES (default, '%s', '%sView%ld%ld', 0, %d, 0, 0.0, 0.0, localtimestamp);",
			recindexname,recmodelname,recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec,numRatings);

		// Now execute the INSERT query.
		recathon_queryExecute(querystring);
		pfree(querystring);
	}
}

/*
 * Create user similarity matrices for each cell in a recommender.
 */
static void userSimilarity(CreateRStmt *recStmt, attr_node attList, int numatts, recMethod method) {
	// For cosine similarity, we will constantly re-use the vector
	// length of each user, and that's a pain to calculate. We're
	// going to pre-calculate that information, store it in an
	// array, and refer to it when necessary.
	int *userIDs;
	float *userLengths;
	// For Pearson correlation.
	float *userAvgs, *userPearsons;
	int i, numUsers;
	// Objects for querying.
	char *querystring;
	QueryDesc *queryDesc;
	PlanState *planstate;
	TupleTableSlot *slot;
	MemoryContext recathoncontext;

	// For cosine, we obtain the list of users, the number of users, and their vector lengths.
	if (method == userCosCF)
		userLengths = vector_lengths(recStmt->usertable->relname,recStmt->userkey,
				recStmt->ratingtable->relname,recStmt->ratingval,&numUsers,&userIDs);

	// For Pearson, we obtain the list of users, the number of users, their average ratings,
	// and another useful constant (I'm calling them Pearsons).
	else if (method == userPearCF)
		pearson_info(recStmt->usertable->relname,recStmt->userkey,recStmt->ratingtable->relname,
			recStmt->ratingval,&numUsers,&userIDs,&userAvgs,&userPearsons);

	// Populate the recindex table, with all the necessary entries.
	// If the recommender is context-based, then we need to obtain
	// all of the possible value combinations, to make an entry
	// for each one. If it's context-free, we just need the one,
	// and no context information necessary.
	if (numatts > 0) {
		attr_node temp_node;
		char *attquerystring;
		char **attnames, **attvalues;

		// We'll temporarily store attribute names in this
		// structure. It's handy to have an array, especially
		// something that corresponds one-to-one with the values
		// structure.
		attnames = (char**) palloc(numatts*sizeof(char*));
		temp_node = attList;
		for (i = 0; i < numatts; i++) {
			attnames[i] = (char*) palloc(256*sizeof(char));
			sprintf(attnames[i],"%s",temp_node->colname);
			temp_node = temp_node->next;
		}

		// We'll store the attribute values in this.
		attvalues = (char**) palloc(numatts*sizeof(char*));

		// We need to select each value combination.
		attquerystring = (char*) palloc(1024*sizeof(char));
		sprintf(attquerystring,"SELECT DISTINCT ");
		for (temp_node = attList; temp_node; temp_node = temp_node->next) {
			strncat(attquerystring,temp_node->colname,strlen(temp_node->colname));
			if (temp_node->next)
				strncat(attquerystring,", ",2);
		}
		strncat(attquerystring," FROM ",6);
		strncat(attquerystring,recStmt->usertable->relname,strlen(recStmt->usertable->relname));
		strncat(attquerystring,";",2);

		// Now that we have a query formulated, we look through the
		// results and insert one entry into the recindex for each
		// result.
		queryDesc = recathon_queryStart(attquerystring, &recathoncontext);
		planstate = queryDesc->planstate;

		// Look through the results of our SELECT DISTINCT query.
		for (;;) {
			int j;
			int numRatings = 0;
			char *recindexname, *recmodelname;
			struct timeval timestamp;

			// Let's quickly fill in the name of the recIndex,
			// since it'll be an argument for a later function.
			recindexname = (char*) palloc((6+strlen(recStmt->relation->relname))*sizeof(char));
			sprintf(recindexname,"%sIndex",recStmt->relation->relname);
printf("1: %s\n",recindexname);
			slot = ExecProcNode(planstate);
			if (TupIsNull(slot)) break;
printf("2: %s\n",recindexname);
			// Scan through the tuple and get all of the attvalues
			// that correspond to our attnames.
			for (j = 0; j < numatts; j++)
				attvalues[j] = getTupleString(slot,attnames[j]);
printf("3: %s\n",recindexname);
			// There's no way a "SELECT DISTINCT" clause is going
			// to turn up NULL values, so our attvalues array should
			// be full now.
			querystring = (char*) palloc(1024*sizeof(char));
			gettimeofday(&timestamp,NULL);
printf("4: %s\n",recindexname);
			// Let's name the model.
			recmodelname = (char*) palloc(256*sizeof(char));
			sprintf(recmodelname,"%sModel%ld%ld",recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec);
printf("5: %s\n",recindexname);
			// We need to create a RecModel and calculate user similarities.
			querystring = (char*) palloc(1024*sizeof(char));
			sprintf(querystring,"CREATE TABLE %s (user1 INTEGER NOT NULL, user2 INTEGER NOT NULL, similarity REAL NOT NULL);",
				recmodelname);
			// Execute the INSERT.
			recathon_utilityExecute(querystring);
printf("6: %s\n",recindexname);
			// Create a blank RecView for use with FILTERRECOMMEND
			// and JOINRECOMMEND, and populate with a blank tuple.
			sprintf(querystring,"CREATE TABLE %sView%ld%ld (%s INTEGER NOT NULL, %s INTEGER NOT NULL, PRIMARY KEY (%s, %s), recscore REAL NOT NULL);",
				recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec,
				recStmt->userkey,recStmt->itemkey,
				recStmt->userkey,recStmt->itemkey);
			recathon_utilityExecute(querystring);
printf("7: %s\n",recindexname);
			// Insert a dummy tuple into the RecView.
			sprintf(querystring,"INSERT INTO %sView%ld%ld VALUES(-1,-1,-1);",
				recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec);
			recathon_queryExecute(querystring);
printf("8: %s\n",recindexname);
			// The task of populating the similarity matrix is left to an
			// external function.
			if (method == userCosCF)
				numRatings = updateUserCosModel(recStmt->usertable->relname,recStmt->itemtable->relname,
							recStmt->ratingtable->relname,recStmt->userkey,recStmt->itemkey,
							recStmt->ratingval,recmodelname,numatts,attnames,attvalues,
							userIDs,userLengths,numUsers,false);
			else if (method == userPearCF)
				numRatings = updateUserPearModel(recStmt->usertable->relname,recStmt->itemtable->relname,
							recStmt->ratingtable->relname,recStmt->userkey,recStmt->itemkey,
							recStmt->ratingval,recmodelname,numatts,attnames,attvalues,
							userIDs,userAvgs,userPearsons,numUsers,false);
printf("9: %s\n",recindexname);
			// Now we can insert an entry into the index table for this cell.
			sprintf(querystring,"INSERT INTO %s VALUES (default, '%s', '%sView%ld%ld', 0, %d, 0, 0.0, 0.0, localtimestamp",
				recindexname,recmodelname,recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec,numRatings);
			// We need to add the attribute values.
			for (i = 0; i < numatts; i++) {
				char addition[256];
				sprintf(addition,", '%s'",attvalues[i]);
				strncat(querystring,addition,strlen(addition));
			}
			strncat(querystring,");",2);
printf("%s\n",querystring);
			// Now execute the INSERT query.
			recathon_queryExecute(querystring);
			pfree(querystring);
		}

		// Some memory cleanup.
		recathon_queryEnd(queryDesc, recathoncontext);
		for (i = 0; i < numatts; i++) {
			pfree(attnames[i]);
			pfree(attvalues[i]);
		}
		pfree(attnames);
		pfree(attvalues);

	// In case our recommender is context-free, our work is much easier.
	} else {
		int numRatings = 0;
		char *recindexname, *recmodelname;
		struct timeval timestamp;

		// Let's quickly fill in the name of the recIndex,
		// since it'll be an argument for a later function.
		recindexname = (char*) palloc((6+strlen(recStmt->relation->relname))*sizeof(char));
		sprintf(recindexname,"%sIndex",recStmt->relation->relname);

		// Inserting the one entry for the recindex.
		querystring = (char*) palloc(1024*sizeof(char));
		gettimeofday(&timestamp,NULL);

		// Let's name the model.
		recmodelname = (char*) palloc(256*sizeof(char));
		sprintf(recmodelname,"%sModel%ld%ld",recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec);

		// We need to create a recmodel and calculate item similarities.
		querystring = (char*) palloc(1024*sizeof(char));
		sprintf(querystring,"CREATE TABLE %s (user1 INTEGER NOT NULL, user2 INTEGER NOT NULL, similarity REAL NOT NULL);",
			recmodelname);
		// Execute the INSERT.
		recathon_utilityExecute(querystring);

		// Create a blank RecView for use with FILTERRECOMMEND
		// and JOINRECOMMEND, and populate with a blank tuple.
		sprintf(querystring,"CREATE TABLE %sView%ld%ld (%s INTEGER NOT NULL, %s INTEGER NOT NULL, PRIMARY KEY (%s, %s), recscore REAL NOT NULL);",
			recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec,
			recStmt->userkey,recStmt->itemkey,
			recStmt->userkey,recStmt->itemkey);
		recathon_utilityExecute(querystring);

		// Insert a dummy tuple into the RecView.
		sprintf(querystring,"INSERT INTO %sView%ld%ld VALUES(-1,-1,-1);",
			recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec);
		recathon_queryExecute(querystring);

		// The task of populating the similarity matrix is left to an
		// external function.
		if (method == userCosCF)
			numRatings = updateUserCosModel(recStmt->usertable->relname,recStmt->itemtable->relname,
						recStmt->ratingtable->relname,recStmt->userkey,recStmt->itemkey,
						recStmt->ratingval,recmodelname,numatts,NULL,NULL,
						userIDs,userLengths,numUsers,false);
		else if (method == userPearCF)
			numRatings = updateUserPearModel(recStmt->usertable->relname,recStmt->itemtable->relname,
						recStmt->ratingtable->relname,recStmt->userkey,recStmt->itemkey,
						recStmt->ratingval,recmodelname,numatts,NULL,NULL,
						userIDs,userAvgs,userPearsons,numUsers,false);

		// Now we can insert an entry into the index table for this cell.
		sprintf(querystring,"INSERT INTO %s VALUES (default, '%s', '%sView%ld%ld', 0, %d, 0, 0.0, 0.0, localtimestamp);",
			recindexname,recmodelname,recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec,numRatings);

		// Now execute the INSERT query.
		recathon_queryExecute(querystring);
		pfree(querystring);
	}
}

/*
 * Create item similarity matrices for each cell in a recommender.
 */
static void SVDSimilarity(CreateRStmt *recStmt, attr_node attList, int numatts) {
	// For cosine similarity, we will constantly re-use the vector
	// length of each item, and that's a pain to calculate. We're
	// going to pre-calculate that information, store it in an
	// array, and refer to it when necessary.
	int i;
	// Objects for querying.
	char *querystring;
	QueryDesc *queryDesc;
	PlanState *planstate;
	TupleTableSlot *slot;
	MemoryContext recathoncontext;

	// Populate the recindex table, with all the necessary entries.
	// If the recommender is context-based, then we need to obtain
	// all of the possible value combinations, to make an entry
	// for each one. If it's context-free, we just need the one,
	// and no context information necessary.
	if (numatts > 0) {
		attr_node temp_node;
		char *attquerystring;
		char **attnames, **attvalues;

		// We'll temporarily store attribute names in this
		// structure. It's handy to have an array, especially
		// something that corresponds one-to-one with the values
		// structure.
		attnames = (char**) palloc(numatts*sizeof(char*));
		temp_node = attList;
		for (i = 0; i < numatts; i++) {
			attnames[i] = (char*) palloc(256*sizeof(char));
			sprintf(attnames[i],"%s",temp_node->colname);
			temp_node = temp_node->next;
		}

		// We'll store the attribute values in this.
		attvalues = (char**) palloc(numatts*sizeof(char*));

		// We need to select each value combination.
		attquerystring = (char*) palloc(1024*sizeof(char));
		sprintf(attquerystring,"SELECT DISTINCT ");
		for (temp_node = attList; temp_node; temp_node = temp_node->next) {
			strncat(attquerystring,temp_node->colname,strlen(temp_node->colname));
			if (temp_node->next)
				strncat(attquerystring,", ",2);
		}
		strncat(attquerystring," FROM ",6);
		strncat(attquerystring,recStmt->usertable->relname,strlen(recStmt->usertable->relname));
		strncat(attquerystring,";",2);

		// Now that we have a query formulated, we look through the
		// results and insert one entry into the recindex for each
		// result.
		queryDesc = recathon_queryStart(attquerystring, &recathoncontext);
		planstate = queryDesc->planstate;

		// Look through the results of our SELECT DISTINCT query.
		for (;;) {
			int j;
			int numRatings = 0;
			char *recindexname, *recusermodelname, *recitemmodelname;
			struct timeval timestamp;

			// Let's quickly fill in the name of the recIndex,
			// since it'll be an argument for a later function.
			recindexname = (char*) palloc((6+strlen(recStmt->relation->relname))*sizeof(char));
			sprintf(recindexname,"%sIndex",recStmt->relation->relname);

			slot = ExecProcNode(planstate);
			if (TupIsNull(slot)) break;

			// Scan through the tuple and get all of the attvalues
			// that correspond to our attnames.
			for (j = 0; j < numatts; j++)
				attvalues[j] = getTupleString(slot,attnames[j]);

			// There's no way a "SELECT DISTINCT" clause is going
			// to turn up NULL values, so our attvalues array should
			// be full now.
			querystring = (char*) palloc(1024*sizeof(char));
			gettimeofday(&timestamp,NULL);

			// Let's name the user model.
			recusermodelname = (char*) palloc(256*sizeof(char));
			sprintf(recusermodelname,"%sUserModel%ld%ld",recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec);

			// Let's name the item model.
			recitemmodelname = (char*) palloc(256*sizeof(char));
			sprintf(recitemmodelname,"%sItemModel%ld%ld",recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec);

			// We need to create two RecModels and do the SVD to populate them.
			querystring = (char*) palloc(1024*sizeof(char));
			sprintf(querystring,"CREATE TABLE %s (users INTEGER NOT NULL, feature INTEGER NOT NULL, value REAL NOT NULL);",
				recusermodelname);
			// Execute the INSERT.
			recathon_utilityExecute(querystring);

			sprintf(querystring,"CREATE TABLE %s (items INTEGER NOT NULL, feature INTEGER NOT NULL, value REAL NOT NULL);",
				recitemmodelname);
			// Execute the INSERT.
			recathon_utilityExecute(querystring);

			// Create a blank RecView for use with FILTERRECOMMEND
			// and JOINRECOMMEND, and populate with a blank tuple.
			sprintf(querystring,"CREATE TABLE %sView%ld%ld (%s INTEGER NOT NULL, %s INTEGER NOT NULL, PRIMARY KEY (%s, %s), recscore REAL NOT NULL);",
				recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec,
				recStmt->userkey,recStmt->itemkey,
				recStmt->userkey,recStmt->itemkey);
			recathon_utilityExecute(querystring);

			// Insert a dummy tuple into the RecView.
			sprintf(querystring,"INSERT INTO %sView%ld%ld VALUES(-1,-1,-1);",
				recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec);
			recathon_queryExecute(querystring);

			// The task of populating the feature matrices is left to
			// an external function.
			numRatings = SVDsimilarity(recStmt->usertable->relname,recStmt->userkey,recStmt->itemtable->relname,
				recStmt->itemkey,recStmt->ratingtable->relname,recStmt->ratingval,
				recusermodelname,recitemmodelname,attnames,attvalues,numatts,false);

			// Now we can insert an entry into the index table for this cell.
			sprintf(querystring,"INSERT INTO %s VALUES (default, '%s', '%s', '%sView%ld%ld', 0, %d, 0, 0.0, 0.0, localtimestamp",
				recindexname,recusermodelname,recitemmodelname,
				recStmt->relation->relname,
				timestamp.tv_sec,timestamp.tv_usec,numRatings);
			// We need to add the attribute values.
			for (i = 0; i < numatts; i++) {
				char addition[256];
				sprintf(addition,", '%s'",attvalues[i]);
				strncat(querystring,addition,strlen(addition));
			}
			strncat(querystring,");",2);
			// Now execute the INSERT query.
			recathon_queryExecute(querystring);
			pfree(querystring);
		}

		// Some memory cleanup.
		recathon_queryEnd(queryDesc, recathoncontext);
		for (i = 0; i < numatts; i++) {
			pfree(attnames[i]);
			pfree(attvalues[i]);
		}
		pfree(attnames);
		pfree(attvalues);

	// In case our recommender is context-free, our work is much easier.
	} else {
		int numRatings = 0;
		char *recindexname, *recusermodelname, *recitemmodelname;
		struct timeval timestamp;

		// Let's quickly fill in the name of the recIndex,
		// since it'll be an argument for a later function.
		recindexname = (char*) palloc((6+strlen(recStmt->relation->relname))*sizeof(char));
		sprintf(recindexname,"%sIndex",recStmt->relation->relname);

		// Inserting the one entry for the recindex.
		querystring = (char*) palloc(1024*sizeof(char));
		gettimeofday(&timestamp,NULL);

		// Let's name the user model.
		recusermodelname = (char*) palloc(256*sizeof(char));
		sprintf(recusermodelname,"%sUserModel%ld%ld",recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec);

		// Let's name the item model.
		recitemmodelname = (char*) palloc(256*sizeof(char));
		sprintf(recitemmodelname,"%sItemModel%ld%ld",recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec);

		// We need to create two RecModels and do the SVD to populate them.
		querystring = (char*) palloc(1024*sizeof(char));
		sprintf(querystring,"CREATE TABLE %s (users INTEGER NOT NULL, feature INTEGER NOT NULL, value REAL NOT NULL);",
			recusermodelname);
		// Execute the INSERT.
		recathon_utilityExecute(querystring);

		sprintf(querystring,"CREATE TABLE %s (items INTEGER NOT NULL, feature INTEGER NOT NULL, value REAL NOT NULL);",
			recitemmodelname);
		// Execute the INSERT.
		recathon_utilityExecute(querystring);

		// Create a blank RecView for use with FILTERRECOMMEND
		// and JOINRECOMMEND, and populate with a blank tuple.
		sprintf(querystring,"CREATE TABLE %sView%ld%ld (%s INTEGER NOT NULL, %s INTEGER NOT NULL, PRIMARY KEY (%s, %s), recscore REAL NOT NULL);",
			recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec,
			recStmt->userkey,recStmt->itemkey,
			recStmt->userkey,recStmt->itemkey);
		recathon_utilityExecute(querystring);

		// Insert a dummy tuple into the RecView.
		sprintf(querystring,"INSERT INTO %sView%ld%ld VALUES(-1,-1,-1);",
			recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec);
		recathon_queryExecute(querystring);

		// The task of populating the feature matrices is left to an
		// external function.
		numRatings = SVDsimilarity(recStmt->usertable->relname,recStmt->userkey,recStmt->itemtable->relname,
			recStmt->itemkey,recStmt->ratingtable->relname,recStmt->ratingval,
			recusermodelname,recitemmodelname,NULL,NULL,0,false);

		// Now we can insert an entry into the index table for this cell.
		sprintf(querystring,"INSERT INTO %s VALUES (default, '%s', '%s', '%sView%ld%ld', 0, %d, 0, 0.0, 0.0, localtimestamp);",
			recindexname,recusermodelname,recitemmodelname,
			recStmt->relation->relname,
			timestamp.tv_sec,timestamp.tv_usec,numRatings);
		// Now execute the INSERT query.
		recathon_queryExecute(querystring);
		pfree(querystring);
	}
}

/*
 * Verify user has ownership of specified relation, else ereport.
 *
 * If noCatalogs is true then we also deny access to system catalogs,
 * except when allowSystemTableMods is true.
 */
void
CheckRelationOwnership(RangeVar *rel, bool noCatalogs)
{
	Oid			relOid;
	HeapTuple	tuple;

	/*
	 * XXX: This is unsafe in the presence of concurrent DDL, since it is
	 * called before acquiring any lock on the target relation.  However,
	 * locking the target relation (especially using something like
	 * AccessExclusiveLock) before verifying that the user has permissions is
	 * not appealing either.
	 */
	relOid = RangeVarGetRelid(rel, NoLock, false);

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))		/* should not happen */
		elog(ERROR, "cache lookup failed for relation %u", relOid);

	if (!pg_class_ownercheck(relOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   rel->relname);

	if (noCatalogs)
	{
		if (!allowSystemTableMods &&
			IsSystemClass((Form_pg_class) GETSTRUCT(tuple)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied: \"%s\" is a system catalog",
							rel->relname)));
	}

	ReleaseSysCache(tuple);
}


/*
 * CommandIsReadOnly: is an executable query read-only?
 *
 * This is a much stricter test than we apply for XactReadOnly mode;
 * the query must be *in truth* read-only, because the caller wishes
 * not to do CommandCounterIncrement for it.
 *
 * Note: currently no need to support Query nodes here
 */
bool
CommandIsReadOnly(Node *parsetree)
{
	if (IsA(parsetree, PlannedStmt))
	{
		PlannedStmt *stmt = (PlannedStmt *) parsetree;

		switch (stmt->commandType)
		{
			case CMD_SELECT:
				if (stmt->rowMarks != NIL)
					return false;		/* SELECT FOR UPDATE/SHARE */
				else if (stmt->hasModifyingCTE)
					return false;		/* data-modifying CTE */
				else
					return true;
			case CMD_UPDATE:
			case CMD_INSERT:
			case CMD_DELETE:
				return false;
			default:
				elog(WARNING, "unrecognized commandType: %d",
					 (int) stmt->commandType);
				break;
		}
	}
	/* For now, treat all utility commands as read/write */
	return false;
}

/*
 * check_xact_readonly: is a utility command read-only?
 *
 * Here we use the loose rules of XactReadOnly mode: no permanent effects
 * on the database are allowed.
 */
static void
check_xact_readonly(Node *parsetree)
{
	if (!XactReadOnly)
		return;

	/*
	 * Note: Commands that need to do more complicated checking are handled
	 * elsewhere, in particular COPY and plannable statements do their own
	 * checking.  However they should all call PreventCommandIfReadOnly to
	 * actually throw the error.
	 */

	switch (nodeTag(parsetree))
	{
		case T_AlterDatabaseStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDomainStmt:
		case T_AlterFunctionStmt:
		case T_AlterRoleStmt:
		case T_AlterRoleSetStmt:
		case T_AlterObjectSchemaStmt:
		case T_AlterOwnerStmt:
		case T_AlterSeqStmt:
		case T_AlterTableStmt:
		case T_RenameStmt:
		case T_CommentStmt:
		case T_DefineStmt:
		case T_CreateCastStmt:
		case T_CreateConversionStmt:
		case T_CreatedbStmt:
		case T_CreateDomainStmt:
		case T_CreateFunctionStmt:
		case T_CreateRoleStmt:
		case T_IndexStmt:
		case T_CreatePLangStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_AlterOpFamilyStmt:
		case T_RuleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateStmt:
		case T_CreateRStmt:
		case T_DropRecStmt:
		case T_CreateTableAsStmt:
		case T_CreateTableSpaceStmt:
		case T_CreateTrigStmt:
		case T_CompositeTypeStmt:
		case T_CreateEnumStmt:
		case T_CreateRangeStmt:
		case T_AlterEnumStmt:
		case T_ViewStmt:
		case T_DropStmt:
		case T_DropdbStmt:
		case T_DropTableSpaceStmt:
		case T_DropRoleStmt:
		case T_GrantStmt:
		case T_GrantRoleStmt:
		case T_AlterDefaultPrivilegesStmt:
		case T_TruncateStmt:
		case T_DropOwnedStmt:
		case T_ReassignOwnedStmt:
		case T_AlterTSDictionaryStmt:
		case T_AlterTSConfigurationStmt:
		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_AlterUserMappingStmt:
		case T_DropUserMappingStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_CreateForeignTableStmt:
		case T_SecLabelStmt:
			PreventCommandIfReadOnly(CreateCommandTag(parsetree));
			break;
		default:
			/* do nothing */
			break;
	}
}

/*
 * PreventCommandIfReadOnly: throw error if XactReadOnly
 *
 * This is useful mainly to ensure consistency of the error message wording;
 * most callers have checked XactReadOnly for themselves.
 */
void
PreventCommandIfReadOnly(const char *cmdname)
{
	if (XactReadOnly)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s in a read-only transaction",
						cmdname)));
}

/*
 * PreventCommandDuringRecovery: throw error if RecoveryInProgress
 *
 * The majority of operations that are unsafe in a Hot Standby slave
 * will be rejected by XactReadOnly tests.	However there are a few
 * commands that are allowed in "read-only" xacts but cannot be allowed
 * in Hot Standby mode.  Those commands should call this function.
 */
void
PreventCommandDuringRecovery(const char *cmdname)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s during recovery",
						cmdname)));
}

/*
 * CheckRestrictedOperation: throw error for hazardous command if we're
 * inside a security restriction context.
 *
 * This is needed to protect session-local state for which there is not any
 * better-defined protection mechanism, such as ownership.
 */
static void
CheckRestrictedOperation(const char *cmdname)
{
	if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		/* translator: %s is name of a SQL command, eg PREPARE */
			 errmsg("cannot execute %s within security-restricted operation",
					cmdname)));
}


/*
 * ProcessUtility
 *		general utility function invoker
 *
 *	parsetree: the parse tree for the utility statement
 *	queryString: original source text of command
 *	params: parameters to use during execution
 *	isTopLevel: true if executing a "top level" (interactively issued) command
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * Notes: as of PG 8.4, caller MUST supply a queryString; it is not
 * allowed anymore to pass NULL.  (If you really don't have source text,
 * you can pass a constant string, perhaps "(query not available)".)
 *
 * completionTag is only set nonempty if we want to return a nondefault status.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
ProcessUtility(Node *parsetree,
			   const char *queryString,
			   ParamListInfo params,
			   bool isTopLevel,
			   DestReceiver *dest,
			   char *completionTag)
{
	Assert(queryString != NULL);	/* required as of 8.4 */

	/*
	 * We provide a function hook variable that lets loadable plugins get
	 * control when ProcessUtility is called.  Such a plugin would normally
	 * call standard_ProcessUtility().
	 */
	if (ProcessUtility_hook)
		(*ProcessUtility_hook) (parsetree, queryString, params,
								isTopLevel, dest, completionTag);
	else
		standard_ProcessUtility(parsetree, queryString, params,
								isTopLevel, dest, completionTag);
}

void
standard_ProcessUtility(Node *parsetree,
						const char *queryString,
						ParamListInfo params,
						bool isTopLevel,
						DestReceiver *dest,
						char *completionTag)
{
	check_xact_readonly(parsetree);

	if (completionTag)
		completionTag[0] = '\0';

	switch (nodeTag(parsetree))
	{
			/*
			 * ******************** transactions ********************
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
						/*
						 * START TRANSACTION, as defined by SQL99: Identical
						 * to BEGIN.  Same code for both.
						 */
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
						{
							ListCell   *lc;

							BeginTransactionBlock();
							foreach(lc, stmt->options)
							{
								DefElem    *item = (DefElem *) lfirst(lc);

								if (strcmp(item->defname, "transaction_isolation") == 0)
									SetPGVariable("transaction_isolation",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_read_only") == 0)
									SetPGVariable("transaction_read_only",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_deferrable") == 0)
									SetPGVariable("transaction_deferrable",
												  list_make1(item->arg),
												  true);
							}
						}
						break;

					case TRANS_STMT_COMMIT:
						if (!EndTransactionBlock())
						{
							/* report unsuccessful commit in completionTag */
							if (completionTag)
								strcpy(completionTag, "ROLLBACK");
						}
						break;

					case TRANS_STMT_PREPARE:
						PreventCommandDuringRecovery("PREPARE TRANSACTION");
						if (!PrepareTransactionBlock(stmt->gid))
						{
							/* report unsuccessful commit in completionTag */
							if (completionTag)
								strcpy(completionTag, "ROLLBACK");
						}
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						PreventTransactionChain(isTopLevel, "COMMIT PREPARED");
						PreventCommandDuringRecovery("COMMIT PREPARED");
						FinishPreparedTransaction(stmt->gid, true);
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						PreventTransactionChain(isTopLevel, "ROLLBACK PREPARED");
						PreventCommandDuringRecovery("ROLLBACK PREPARED");
						FinishPreparedTransaction(stmt->gid, false);
						break;

					case TRANS_STMT_ROLLBACK:
						UserAbortTransactionBlock();
						break;

					case TRANS_STMT_SAVEPOINT:
						{
							ListCell   *cell;
							char	   *name = NULL;

							RequireTransactionChain(isTopLevel, "SAVEPOINT");

							foreach(cell, stmt->options)
							{
								DefElem    *elem = lfirst(cell);

								if (strcmp(elem->defname, "savepoint_name") == 0)
									name = strVal(elem->arg);
							}

							Assert(PointerIsValid(name));

							DefineSavepoint(name);
						}
						break;

					case TRANS_STMT_RELEASE:
						RequireTransactionChain(isTopLevel, "RELEASE SAVEPOINT");
						ReleaseSavepoint(stmt->options);
						break;

					case TRANS_STMT_ROLLBACK_TO:
						RequireTransactionChain(isTopLevel, "ROLLBACK TO SAVEPOINT");
						RollbackToSavepoint(stmt->options);

						/*
						 * CommitTransactionCommand is in charge of
						 * re-defining the savepoint again
						 */
						break;
				}
			}
			break;

			/*
			 * Portal (cursor) manipulation
			 *
			 * Note: DECLARE CURSOR is processed mostly as a SELECT, and
			 * therefore what we will get here is a PlannedStmt not a bare
			 * DeclareCursorStmt.
			 */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				if (stmt->utilityStmt == NULL ||
					!IsA(stmt->utilityStmt, DeclareCursorStmt))
					elog(ERROR, "non-DECLARE CURSOR PlannedStmt passed to ProcessUtility");
				PerformCursorOpen(stmt, params, queryString, isTopLevel);
			}
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				CheckRestrictedOperation("CLOSE");
				PerformPortalClose(stmt->portalname);
			}
			break;

		case T_FetchStmt:
			PerformPortalFetch((FetchStmt *) parsetree, dest,
							   completionTag);
			break;

			/*
			 * relation and attribute manipulation
			 */
		case T_CreateSchemaStmt:
			CreateSchemaCommand((CreateSchemaStmt *) parsetree,
								queryString);
			break;

		case T_CreateStmt:
		case T_CreateForeignTableStmt:
			{
				List	   *stmts;
				ListCell   *l;
				Oid			relOid;

				/* Run parse analysis ... */
				stmts = transformCreateStmt((CreateStmt *) parsetree,
											queryString);

				/* ... and do it */
				foreach(l, stmts)
				{
					Node	   *stmt = (Node *) lfirst(l);

					if (IsA(stmt, CreateStmt))
					{
						Datum		toast_options;
						static char *validnsps[] = HEAP_RELOPT_NAMESPACES;

						/* Create the table itself */
						relOid = DefineRelation((CreateStmt *) stmt,
												RELKIND_RELATION,
												InvalidOid);

						/*
						 * Let AlterTableCreateToastTable decide if this one
						 * needs a secondary relation too.
						 */
						CommandCounterIncrement();

						/* parse and validate reloptions for the toast table */
						toast_options = transformRelOptions((Datum) 0,
											  ((CreateStmt *) stmt)->options,
															"toast",
															validnsps,
															true, false);
						(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options,
											   true);

						AlterTableCreateToastTable(relOid, toast_options);
					}
					else if (IsA(stmt, CreateForeignTableStmt))
					{
						/* Create the table itself */
						relOid = DefineRelation((CreateStmt *) stmt,
												RELKIND_FOREIGN_TABLE,
												InvalidOid);
						CreateForeignTable((CreateForeignTableStmt *) stmt,
										   relOid);
					}
					else
					{
						/* Recurse for anything else */
						ProcessUtility(stmt,
									   queryString,
									   params,
									   false,
									   None_Receiver,
									   NULL);
					}

					/* Need CCI between commands */
					if (lnext(l) != NULL)
						CommandCounterIncrement();
				}
			}
			break;

		// NEW FOR RECATHON
		// If we come across this case, someone has tried to create a
		// recommender. We double-check all the data to make sure
		// everything is valid, then send it off to Recathon.
		case T_CreateRStmt:
			{
				CreateRStmt* recStmt;
				int numatts;
				recMethod method;
				char *querystring;
				attr_node attr_list, temp_attr;
				RangeVar *proprv;

				recStmt = (CreateRStmt*) parsetree;

				/*
				 * SANITY CHECKS
				 *
				 * Before we send the data to Recathon, we need to implement a series
				 * of sanity checks, to make sure the data is actually usable.
				 */

				attr_list = validateCreateRStmt(recStmt, &method, &numatts);

				/*
				 * CREATE TABLES
				 *
				 * The next step is to create the tables that we will be using for
				 * our recommenders. This includes a RecModelsCatalogue (unless it
				 * exists), a RecathonProperties table, for various parameters
				 * (also unless it exists), and the recIndex table.
				 */

				// Create the RecModelsCatalogue table, if it doesn't exist.
				querystring = (char*) palloc(512*sizeof(char));
				sprintf(querystring,"CREATE TABLE IF NOT EXISTS RecModelsCatalogue (recommenderId serial, PRIMARY KEY (recommenderId), recommenderIndexName VARCHAR NOT NULL, userTable VARCHAR NOT NULL, itemTable VARCHAR NOT NULL, ratingTable VARCHAR NOT NULL, userKey VARCHAR NOT NULL, itemKey VARCHAR NOT NULL, ratingVal VARCHAR NOT NULL, method VARCHAR NOT NULL, contextattributes INTEGER NOT NULL);");
				recathon_utilityExecute(querystring);
				pfree(querystring);

				// Insert recommender information into the RecModelsCatalogue.
				querystring = (char*) palloc(1024*sizeof(char));
				sprintf(querystring,"INSERT INTO RecModelsCatalogue VALUES (default,'%sIndex','%s','%s','%s','%s','%s','%s','%s',%d);",
					recStmt->relation->relname, recStmt->usertable->relname, recStmt->itemtable->relname,
					recStmt->ratingtable->relname, recStmt->userkey, recStmt->itemkey, recStmt->ratingval,
					recStmt->method, numatts);

				// Execute the query and add this recommender to the catalogue.
				recathon_queryExecute(querystring);
				pfree(querystring);

				// Create the RecathonProperties table, if it doesn't exist.
				// Here we do an actual check on the table, because we want
				// to avoid both the table creation and the insert.
				proprv = makeRangeVar(NULL,"recathonproperties",0);
				if (!relationExists(proprv)) {
					querystring = (char*) palloc(256*sizeof(char));
					sprintf(querystring,"CREATE TABLE RecathonProperties (update_threshold REAL NOT NULL, tail_length INTEGER NOT NULL, verbose_queries BOOLEAN NOT NULL);");
					recathon_utilityExecute(querystring);
					pfree(querystring);

					// Insert default values into the table.
					querystring = (char*) palloc(64*sizeof(char));
					sprintf(querystring,"INSERT INTO RecathonProperties VALUES (0.5, 0, true);");
					recathon_queryExecute(querystring);
					pfree(querystring);
				}
				pfree(proprv);

				// Create the recindex table.
				querystring = (char*) palloc(1024*sizeof(char));
				// SVD uses two different models, while the other methods use only one.
				// Our index schema will differ as a result.
				if (method == SVD) {
					sprintf(querystring,"CREATE TABLE %sIndex (systemId serial, PRIMARY KEY (systemId), recUserModelName VARCHAR NOT NULL, recItemModelName VARCHAR NOT NULL, recViewName VARCHAR NOT NULL, updateCounter INTEGER NOT NULL, ratingTotal INTEGER NOT NULL, queryCounter INTEGER NOT NULL, updateRate REAL NOT NULL, queryRate REAL NOT NULL, levelone_timestamp TIMESTAMP NOT NULL",
						recStmt->relation->relname);
				} else {
					sprintf(querystring,"CREATE TABLE %sIndex (systemId serial, PRIMARY KEY (systemId), recModelName VARCHAR NOT NULL, recViewName VARCHAR NOT NULL, updateCounter INTEGER NOT NULL, ratingTotal INTEGER NOT NULL, queryCounter INTEGER NOT NULL, updateRate REAL NOT NULL, queryRate REAL NOT NULL, levelone_timestamp TIMESTAMP NOT NULL",
						recStmt->relation->relname);
				}
				// Add in the context attributes, if any.
				for (temp_attr = attr_list; temp_attr; temp_attr = temp_attr->next) {
					char addition[64];
					sprintf(addition,", %s VARCHAR NOT NULL",temp_attr->colname);
					strncat(querystring,addition,strlen(addition));
				}
				strncat(querystring,");",2);
				recathon_utilityExecute(querystring);
				pfree(querystring);

				/*
				 * CREATE MODELS
				 *
				 * Finally, we need to create all of the recModels and recViews
				 * we need for the recommender to operate.
				 */
				switch (method) {
					case itemCosCF:
					case itemPearCF:
						itemSimilarity(recStmt,attr_list,numatts,method);
						break;
					case userCosCF:
					case userPearCF:
						userSimilarity(recStmt,attr_list,numatts,method);
						break;
					case SVD:
						SVDSimilarity(recStmt,attr_list,numatts);
						break;
					default:
						ereport(ERROR,
							(errcode(ERRCODE_CASE_NOT_FOUND),
							 errmsg("recommendation method %d not recognized",
								(int)method)));
						break;
				}

				// Free extra memory we used.
				freeAttributes(attr_list);

				break;
			}

		// In case someone wants to drop a recommender.
		case T_DropRecStmt:
			{
				DropRecStmt *dropstmt;
				int i;
				char *query_string, *drop_string, *recname, *recindexname, *method;
				cell_node chead, ctail, ctemp;
				RangeVar *cataloguerv;
				// Query objects.
				QueryDesc *queryDesc;
				PlanState *planstate;
				TupleTableSlot *slot;
				MemoryContext recathoncontext;

				dropstmt = (DropRecStmt *) parsetree;
				recname = dropstmt->recommender->relname;
				// Convert to lowercase.
				for (i = 0; i < strlen(recname); i++)
					recname[i] = tolower(recname[i]);

				// The first thing we do is check to see if this is actually
				// a recommender.
				cataloguerv = makeRangeVar(NULL,"recmodelscatalogue",0);
				if (!relationExists(cataloguerv)) {
					ereport(ERROR,
						(errcode(ERRCODE_INVALID_SCHEMA_NAME),
						 errmsg("no recommenders have been created")));
				}
				pfree(cataloguerv);

				if (!recommenderExists(recname))
					ereport(ERROR,
						(errcode(ERRCODE_INVALID_SCHEMA_NAME),
						 errmsg("recommender %s does not exist",recname)));

				// The first thing we need to know is the recommendation method
				// used. SVD vs. CF will make a difference.
				recindexname = (char*) palloc((strlen(recname)+6)*sizeof(char));
				sprintf(recindexname,"%sIndex",recname);
				getRecInfo(recindexname,NULL,NULL,NULL,NULL,NULL,NULL,&method,NULL);
				pfree(recindexname);

				// We need to run a query on the provided recommender
				// to find all the RecView files we need to delete,
				// both from the database and the recathondata folder.
				chead = NULL; ctail = NULL;
				query_string = (char*) palloc(1024*sizeof(char));
				sprintf(query_string,"select * from %sindex;",recname);

				queryDesc = recathon_queryStart(query_string, &recathoncontext);
				planstate = queryDesc->planstate;

				// Look through query results, storing every cell.
				for (;;) {
					cell_node new_cell;

					slot = ExecProcNode(planstate);
					if (TupIsNull(slot)) break;

					new_cell = (cell_node) palloc(sizeof(struct cell_node_t));
					new_cell->next = NULL;

					// Go through this tuple and get the appropriate information.
					// This will change depending on the recommendation method.
					if (strcmp(method,"svd") == 0) {
						new_cell->modelname1 = getTupleString(slot,"recusermodelname");
						new_cell->modelname2 = getTupleString(slot,"recitemmodelname");
					} else {
						new_cell->modelname1 = getTupleString(slot,"recmodelname");
						new_cell->modelname2 = NULL;
					}
					new_cell->viewname = getTupleString(slot,"recviewname");

					// Insert the new cell_node into our list.
					if (!chead) {
						chead = new_cell;
						ctail = new_cell;
					} else {
						ctail->next = new_cell;
						ctail = new_cell;
					}
				}

				// Now to tidy up.
				recathon_queryEnd(queryDesc, recathoncontext);
				pfree(query_string);

				// Quick error check.
				if (!chead) {
					ereport(WARNING,
						(errcode(ERRCODE_INVALID_SCHEMA_NAME),
						 errmsg("failed to find cells for recommender %s",recname)));
				}

				// For every cell, we need to remove all associated views and tables,
				// then delete the RecView file from the recathondata folder.
				for (ctemp = chead; ctemp; ctemp = ctemp->next) {
					drop_string = (char*) palloc(512*sizeof(char));

					// Delete the first recmodel.
					if (ctemp->modelname1) {
						sprintf(drop_string,"drop table %s;",ctemp->modelname1);
					recathon_utilityExecute(drop_string);
					}

					// Delete the second recmodel.
					if (ctemp->modelname2) {
						sprintf(drop_string,"drop table %s;",ctemp->modelname2);
					recathon_utilityExecute(drop_string);
					}

					// Delete the recview.
					if (ctemp->viewname) {
						sprintf(drop_string,"drop table %s;",ctemp->viewname);
					recathon_utilityExecute(drop_string);
					}

					pfree(drop_string);
				}

				// Now we can free up our cell list.
				freeCellList(chead);

				// We need to do two more removals: we need to remove the recommender
				// index table, and then delete it from the recmodelscatalogue.
				drop_string = (char*) palloc(512*sizeof(char));
				sprintf(drop_string,"drop table %sindex;",recname);
				recathon_utilityExecute(drop_string);

				// Now for the recmodelscatalogue.
				sprintf(drop_string,"delete from recmodelscatalogue where recommenderindexname = '%sIndex';",
						recname);
				recathon_queryExecute(drop_string);

				pfree(drop_string);
			}
			break;

		case T_CreateTableSpaceStmt:
			PreventTransactionChain(isTopLevel, "CREATE TABLESPACE");
			CreateTableSpace((CreateTableSpaceStmt *) parsetree);
			break;

		case T_DropTableSpaceStmt:
			PreventTransactionChain(isTopLevel, "DROP TABLESPACE");
			DropTableSpace((DropTableSpaceStmt *) parsetree);
			break;

		case T_AlterTableSpaceOptionsStmt:
			AlterTableSpaceOptions((AlterTableSpaceOptionsStmt *) parsetree);
			break;

		case T_CreateExtensionStmt:
			CreateExtension((CreateExtensionStmt *) parsetree);
			break;

		case T_AlterExtensionStmt:
			ExecAlterExtensionStmt((AlterExtensionStmt *) parsetree);
			break;

		case T_AlterExtensionContentsStmt:
			ExecAlterExtensionContentsStmt((AlterExtensionContentsStmt *) parsetree);
			break;

		case T_CreateFdwStmt:
			CreateForeignDataWrapper((CreateFdwStmt *) parsetree);
			break;

		case T_AlterFdwStmt:
			AlterForeignDataWrapper((AlterFdwStmt *) parsetree);
			break;

		case T_CreateForeignServerStmt:
			CreateForeignServer((CreateForeignServerStmt *) parsetree);
			break;

		case T_AlterForeignServerStmt:
			AlterForeignServer((AlterForeignServerStmt *) parsetree);
			break;

		case T_CreateUserMappingStmt:
			CreateUserMapping((CreateUserMappingStmt *) parsetree);
			break;

		case T_AlterUserMappingStmt:
			AlterUserMapping((AlterUserMappingStmt *) parsetree);
			break;

		case T_DropUserMappingStmt:
			RemoveUserMapping((DropUserMappingStmt *) parsetree);
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case OBJECT_INDEX:
					if (((DropStmt *) parsetree)->concurrent)
						PreventTransactionChain(isTopLevel,
												"DROP INDEX CONCURRENTLY");
					/* fall through */

				case OBJECT_TABLE:
				case OBJECT_SEQUENCE:
				case OBJECT_VIEW:
				case OBJECT_FOREIGN_TABLE:
					RemoveRelations((DropStmt *) parsetree);
					break;
				default:
					RemoveObjects((DropStmt *) parsetree);
					break;
			}
			break;

		case T_TruncateStmt:
			ExecuteTruncate((TruncateStmt *) parsetree);
			break;

		case T_CommentStmt:
			CommentObject((CommentStmt *) parsetree);
			break;

		case T_SecLabelStmt:
			ExecSecLabelStmt((SecLabelStmt *) parsetree);
			break;

		case T_CopyStmt:
			{
				uint64		processed;

				processed = DoCopy((CopyStmt *) parsetree, queryString);
				if (completionTag)
					snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
							 "COPY " UINT64_FORMAT, processed);
			}
			break;

		case T_PrepareStmt:
			CheckRestrictedOperation("PREPARE");
			PrepareQuery((PrepareStmt *) parsetree, queryString);
			break;

		case T_ExecuteStmt:
			ExecuteQuery((ExecuteStmt *) parsetree, NULL,
						 queryString, params,
						 dest, completionTag);
			break;

		case T_DeallocateStmt:
			CheckRestrictedOperation("DEALLOCATE");
			DeallocateQuery((DeallocateStmt *) parsetree);
			break;

			/*
			 * schema
			 */
		case T_RenameStmt:
			ExecRenameStmt((RenameStmt *) parsetree);
			break;

		case T_AlterObjectSchemaStmt:
			ExecAlterObjectSchemaStmt((AlterObjectSchemaStmt *) parsetree);
			break;

		case T_AlterOwnerStmt:
			ExecAlterOwnerStmt((AlterOwnerStmt *) parsetree);
			break;

		case T_AlterTableStmt:
			{
				AlterTableStmt *atstmt = (AlterTableStmt *) parsetree;
				Oid			relid;
				List	   *stmts;
				ListCell   *l;
				LOCKMODE	lockmode;

				/*
				 * Figure out lock mode, and acquire lock.	This also does
				 * basic permissions checks, so that we won't wait for a lock
				 * on (for example) a relation on which we have no
				 * permissions.
				 */
				lockmode = AlterTableGetLockLevel(atstmt->cmds);
				relid = AlterTableLookupRelation(atstmt, lockmode);

				if (OidIsValid(relid))
				{
					/* Run parse analysis ... */
					stmts = transformAlterTableStmt(atstmt, queryString);

					/* ... and do it */
					foreach(l, stmts)
					{
						Node	   *stmt = (Node *) lfirst(l);

						if (IsA(stmt, AlterTableStmt))
						{
							/* Do the table alteration proper */
							AlterTable(relid, lockmode, (AlterTableStmt *) stmt);
						}
						else
						{
							/* Recurse for anything else */
							ProcessUtility(stmt,
										   queryString,
										   params,
										   false,
										   None_Receiver,
										   NULL);
						}

						/* Need CCI between commands */
						if (lnext(l) != NULL)
							CommandCounterIncrement();
					}
				}
				else
					ereport(NOTICE,
						  (errmsg("relation \"%s\" does not exist, skipping",
								  atstmt->relation->relname)));
			}
			break;

		case T_AlterDomainStmt:
			{
				AlterDomainStmt *stmt = (AlterDomainStmt *) parsetree;

				/*
				 * Some or all of these functions are recursive to cover
				 * inherited things, so permission checks are done there.
				 */
				switch (stmt->subtype)
				{
					case 'T':	/* ALTER DOMAIN DEFAULT */

						/*
						 * Recursively alter column default for table and, if
						 * requested, for descendants
						 */
						AlterDomainDefault(stmt->typeName,
										   stmt->def);
						break;
					case 'N':	/* ALTER DOMAIN DROP NOT NULL */
						AlterDomainNotNull(stmt->typeName,
										   false);
						break;
					case 'O':	/* ALTER DOMAIN SET NOT NULL */
						AlterDomainNotNull(stmt->typeName,
										   true);
						break;
					case 'C':	/* ADD CONSTRAINT */
						AlterDomainAddConstraint(stmt->typeName,
												 stmt->def);
						break;
					case 'X':	/* DROP CONSTRAINT */
						AlterDomainDropConstraint(stmt->typeName,
												  stmt->name,
												  stmt->behavior,
												  stmt->missing_ok);
						break;
					case 'V':	/* VALIDATE CONSTRAINT */
						AlterDomainValidateConstraint(stmt->typeName,
													  stmt->name);
						break;
					default:	/* oops */
						elog(ERROR, "unrecognized alter domain type: %d",
							 (int) stmt->subtype);
						break;
				}
			}
			break;

		case T_GrantStmt:
			ExecuteGrantStmt((GrantStmt *) parsetree);
			break;

		case T_GrantRoleStmt:
			GrantRole((GrantRoleStmt *) parsetree);
			break;

		case T_AlterDefaultPrivilegesStmt:
			ExecAlterDefaultPrivilegesStmt((AlterDefaultPrivilegesStmt *) parsetree);
			break;

			/*
			 * **************** object creation / destruction *****************
			 */
		case T_DefineStmt:
			{
				DefineStmt *stmt = (DefineStmt *) parsetree;

				switch (stmt->kind)
				{
					case OBJECT_AGGREGATE:
						DefineAggregate(stmt->defnames, stmt->args,
										stmt->oldstyle, stmt->definition);
						break;
					case OBJECT_OPERATOR:
						Assert(stmt->args == NIL);
						DefineOperator(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TYPE:
						Assert(stmt->args == NIL);
						DefineType(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSPARSER:
						Assert(stmt->args == NIL);
						DefineTSParser(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSDICTIONARY:
						Assert(stmt->args == NIL);
						DefineTSDictionary(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSTEMPLATE:
						Assert(stmt->args == NIL);
						DefineTSTemplate(stmt->defnames, stmt->definition);
						break;
					case OBJECT_TSCONFIGURATION:
						Assert(stmt->args == NIL);
						DefineTSConfiguration(stmt->defnames, stmt->definition);
						break;
					case OBJECT_COLLATION:
						Assert(stmt->args == NIL);
						DefineCollation(stmt->defnames, stmt->definition);
						break;
					default:
						elog(ERROR, "unrecognized define stmt type: %d",
							 (int) stmt->kind);
						break;
				}
			}
			break;

		case T_CompositeTypeStmt:		/* CREATE TYPE (composite) */
			{
				CompositeTypeStmt *stmt = (CompositeTypeStmt *) parsetree;

				DefineCompositeType(stmt->typevar, stmt->coldeflist);
			}
			break;

		case T_CreateEnumStmt:	/* CREATE TYPE AS ENUM */
			DefineEnum((CreateEnumStmt *) parsetree);
			break;

		case T_CreateRangeStmt:	/* CREATE TYPE AS RANGE */
			DefineRange((CreateRangeStmt *) parsetree);
			break;

		case T_AlterEnumStmt:	/* ALTER TYPE (enum) */

			/*
			 * We disallow this in transaction blocks, because we can't cope
			 * with enum OID values getting into indexes and then having their
			 * defining pg_enum entries go away.
			 */
			PreventTransactionChain(isTopLevel, "ALTER TYPE ... ADD");
			AlterEnum((AlterEnumStmt *) parsetree);
			break;

		case T_ViewStmt:		/* CREATE VIEW */
			DefineView((ViewStmt *) parsetree, queryString);
			break;

		case T_CreateFunctionStmt:		/* CREATE FUNCTION */
			CreateFunction((CreateFunctionStmt *) parsetree, queryString);
			break;

		case T_AlterFunctionStmt:		/* ALTER FUNCTION */
			AlterFunction((AlterFunctionStmt *) parsetree);
			break;

		case T_IndexStmt:		/* CREATE INDEX */
			{
				IndexStmt  *stmt = (IndexStmt *) parsetree;

				if (stmt->concurrent)
					PreventTransactionChain(isTopLevel,
											"CREATE INDEX CONCURRENTLY");

				CheckRelationOwnership(stmt->relation, true);

				/* Run parse analysis ... */
				stmt = transformIndexStmt(stmt, queryString);

				/* ... and do it */
				DefineIndex(stmt,
							InvalidOid, /* no predefined OID */
							false,		/* is_alter_table */
							true,		/* check_rights */
							false,		/* skip_build */
							false);		/* quiet */
			}
			break;

		case T_RuleStmt:		/* CREATE RULE */
			DefineRule((RuleStmt *) parsetree, queryString);
			break;

		case T_CreateSeqStmt:
			DefineSequence((CreateSeqStmt *) parsetree);
			break;

		case T_AlterSeqStmt:
			AlterSequence((AlterSeqStmt *) parsetree);
			break;

		case T_DoStmt:
			ExecuteDoStmt((DoStmt *) parsetree);
			break;

		case T_CreatedbStmt:
			PreventTransactionChain(isTopLevel, "CREATE DATABASE");
			createdb((CreatedbStmt *) parsetree);
			break;

		case T_AlterDatabaseStmt:
			AlterDatabase((AlterDatabaseStmt *) parsetree, isTopLevel);
			break;

		case T_AlterDatabaseSetStmt:
			AlterDatabaseSet((AlterDatabaseSetStmt *) parsetree);
			break;

		case T_DropdbStmt:
			{
				DropdbStmt *stmt = (DropdbStmt *) parsetree;

				PreventTransactionChain(isTopLevel, "DROP DATABASE");
				dropdb(stmt->dbname, stmt->missing_ok);
			}
			break;

			/* Query-level asynchronous notification */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				PreventCommandDuringRecovery("NOTIFY");
				Async_Notify(stmt->conditionname, stmt->payload);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				PreventCommandDuringRecovery("LISTEN");
				CheckRestrictedOperation("LISTEN");
				Async_Listen(stmt->conditionname);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				PreventCommandDuringRecovery("UNLISTEN");
				CheckRestrictedOperation("UNLISTEN");
				if (stmt->conditionname)
					Async_Unlisten(stmt->conditionname);
				else
					Async_UnlistenAll();
			}
			break;

		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				closeAllVfds(); /* probably not necessary... */
				/* Allowed names are restricted if you're not superuser */
				load_file(stmt->filename, !superuser());
			}
			break;

		case T_ClusterStmt:
			/* we choose to allow this during "read only" transactions */
			PreventCommandDuringRecovery("CLUSTER");
			cluster((ClusterStmt *) parsetree, isTopLevel);
			break;

		case T_VacuumStmt:
			/* we choose to allow this during "read only" transactions */
			PreventCommandDuringRecovery("VACUUM");
			vacuum((VacuumStmt *) parsetree, InvalidOid, true, NULL, false,
				   isTopLevel);
			break;

		case T_ExplainStmt:
			ExplainQuery((ExplainStmt *) parsetree, queryString, params, dest);
			break;

		case T_CreateTableAsStmt:
			ExecCreateTableAs((CreateTableAsStmt *) parsetree,
							  queryString, params, completionTag);
			break;

		case T_VariableSetStmt:
			ExecSetVariableStmt((VariableSetStmt *) parsetree);
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name, dest);
			}
			break;

		case T_DiscardStmt:
			/* should we allow DISCARD PLANS? */
			CheckRestrictedOperation("DISCARD");
			DiscardCommand((DiscardStmt *) parsetree, isTopLevel);
			break;

		case T_CreateTrigStmt:
			(void) CreateTrigger((CreateTrigStmt *) parsetree, queryString,
								 InvalidOid, InvalidOid, false);
			break;

		case T_CreatePLangStmt:
			CreateProceduralLanguage((CreatePLangStmt *) parsetree);
			break;

			/*
			 * ******************************** DOMAIN statements ****
			 */
		case T_CreateDomainStmt:
			DefineDomain((CreateDomainStmt *) parsetree);
			break;

			/*
			 * ******************************** ROLE statements ****
			 */
		case T_CreateRoleStmt:
			CreateRole((CreateRoleStmt *) parsetree);
			break;

		case T_AlterRoleStmt:
			AlterRole((AlterRoleStmt *) parsetree);
			break;

		case T_AlterRoleSetStmt:
			AlterRoleSet((AlterRoleSetStmt *) parsetree);
			break;

		case T_DropRoleStmt:
			DropRole((DropRoleStmt *) parsetree);
			break;

		case T_DropOwnedStmt:
			DropOwnedObjects((DropOwnedStmt *) parsetree);
			break;

		case T_ReassignOwnedStmt:
			ReassignOwnedObjects((ReassignOwnedStmt *) parsetree);
			break;

		case T_LockStmt:

			/*
			 * Since the lock would just get dropped immediately, LOCK TABLE
			 * outside a transaction block is presumed to be user error.
			 */
			RequireTransactionChain(isTopLevel, "LOCK TABLE");
			LockTableCommand((LockStmt *) parsetree);
			break;

		case T_ConstraintsSetStmt:
			AfterTriggerSetState((ConstraintsSetStmt *) parsetree);
			break;

		case T_CheckPointStmt:
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("must be superuser to do CHECKPOINT")));

			/*
			 * You might think we should have a PreventCommandDuringRecovery()
			 * here, but we interpret a CHECKPOINT command during recovery as
			 * a request for a restartpoint instead. We allow this since it
			 * can be a useful way of reducing switchover time when using
			 * various forms of replication.
			 */
			RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_WAIT |
							  (RecoveryInProgress() ? 0 : CHECKPOINT_FORCE));
			break;

		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parsetree;

				/* we choose to allow this during "read only" transactions */
				PreventCommandDuringRecovery("REINDEX");
				switch (stmt->kind)
				{
					case OBJECT_INDEX:
						ReindexIndex(stmt->relation);
						break;
					case OBJECT_TABLE:
						ReindexTable(stmt->relation);
						break;
					case OBJECT_DATABASE:

						/*
						 * This cannot run inside a user transaction block; if
						 * we were inside a transaction, then its commit- and
						 * start-transaction-command calls would not have the
						 * intended effect!
						 */
						PreventTransactionChain(isTopLevel,
												"REINDEX DATABASE");
						ReindexDatabase(stmt->name,
										stmt->do_system, stmt->do_user);
						break;
					default:
						elog(ERROR, "unrecognized object type: %d",
							 (int) stmt->kind);
						break;
				}
				break;
			}
			break;

		case T_CreateConversionStmt:
			CreateConversionCommand((CreateConversionStmt *) parsetree);
			break;

		case T_CreateCastStmt:
			CreateCast((CreateCastStmt *) parsetree);
			break;

		case T_CreateOpClassStmt:
			DefineOpClass((CreateOpClassStmt *) parsetree);
			break;

		case T_CreateOpFamilyStmt:
			DefineOpFamily((CreateOpFamilyStmt *) parsetree);
			break;

		case T_AlterOpFamilyStmt:
			AlterOpFamily((AlterOpFamilyStmt *) parsetree);
			break;

		case T_AlterTSDictionaryStmt:
			AlterTSDictionary((AlterTSDictionaryStmt *) parsetree);
			break;

		case T_AlterTSConfigurationStmt:
			AlterTSConfiguration((AlterTSConfigurationStmt *) parsetree);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			break;
	}
}

/*
 * UtilityReturnsTuples
 *		Return "true" if this utility statement will send output to the
 *		destination.
 *
 * Generally, there should be a case here for each case in ProcessUtility
 * where "dest" is passed on.
 */
bool
UtilityReturnsTuples(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return false;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return false;		/* not our business to raise error */
				return portal->tupDesc ? true : false;
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return false;		/* not our business to raise error */
				if (entry->plansource->resultDesc)
					return true;
				return false;
			}

		case T_ExplainStmt:
			return true;

		case T_VariableShowStmt:
			return true;

		default:
			return false;
	}
}

/*
 * UtilityTupleDescriptor
 *		Fetch the actual output tuple descriptor for a utility statement
 *		for which UtilityReturnsTuples() previously returned "true".
 *
 * The returned descriptor is created in (or copied into) the current memory
 * context.
 */
TupleDesc
UtilityTupleDescriptor(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return NULL;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return NULL;	/* not our business to raise error */
				return CreateTupleDescCopy(portal->tupDesc);
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return NULL;	/* not our business to raise error */
				return FetchPreparedStatementResultDesc(entry);
			}

		case T_ExplainStmt:
			return ExplainResultDesc((ExplainStmt *) parsetree);

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				return GetPGVariableResultDesc(n->name);
			}

		default:
			return NULL;
	}
}


/*
 * QueryReturnsTuples
 *		Return "true" if this Query will send output to the destination.
 */
#ifdef NOT_USED
bool
QueryReturnsTuples(Query *parsetree)
{
	switch (parsetree->commandType)
	{
		case CMD_SELECT:
			/* returns tuples ... unless it's DECLARE CURSOR */
			if (parsetree->utilityStmt == NULL)
				return true;
			break;
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
			/* the forms with RETURNING return tuples */
			if (parsetree->returningList)
				return true;
			break;
		case CMD_UTILITY:
			return UtilityReturnsTuples(parsetree->utilityStmt);
		case CMD_UNKNOWN:
		case CMD_NOTHING:
			/* probably shouldn't get here */
			break;
	}
	return false;				/* default */
}
#endif


/*
 * UtilityContainsQuery
 *		Return the contained Query, or NULL if there is none
 *
 * Certain utility statements, such as EXPLAIN, contain a plannable Query.
 * This function encapsulates knowledge of exactly which ones do.
 * We assume it is invoked only on already-parse-analyzed statements
 * (else the contained parsetree isn't a Query yet).
 *
 * In some cases (currently, only EXPLAIN of CREATE TABLE AS/SELECT INTO),
 * potentially Query-containing utility statements can be nested.  This
 * function will drill down to a non-utility Query, or return NULL if none.
 */
Query *
UtilityContainsQuery(Node *parsetree)
{
	Query	   *qry;

	switch (nodeTag(parsetree))
	{
		case T_ExplainStmt:
			qry = (Query *) ((ExplainStmt *) parsetree)->query;
			Assert(IsA(qry, Query));
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		case T_CreateTableAsStmt:
			/* might or might not contain a Query ... */
			qry = (Query *) ((CreateTableAsStmt *) parsetree)->query;
			if (IsA(qry, Query))
			{
				/* Recursion currently can't be necessary here */
				Assert(qry->commandType != CMD_UTILITY);
				return qry;
			}
			Assert(IsA(qry, ExecuteStmt));
			return NULL;

		default:
			return NULL;
	}
}


/*
 * AlterObjectTypeCommandTag
 *		helper function for CreateCommandTag
 *
 * This covers most cases where ALTER is used with an ObjectType enum.
 */
static const char *
AlterObjectTypeCommandTag(ObjectType objtype)
{
	const char *tag;

	switch (objtype)
	{
		case OBJECT_AGGREGATE:
			tag = "ALTER AGGREGATE";
			break;
		case OBJECT_ATTRIBUTE:
			tag = "ALTER TYPE";
			break;
		case OBJECT_CAST:
			tag = "ALTER CAST";
			break;
		case OBJECT_COLLATION:
			tag = "ALTER COLLATION";
			break;
		case OBJECT_COLUMN:
			tag = "ALTER TABLE";
			break;
		case OBJECT_CONSTRAINT:
			tag = "ALTER TABLE";
			break;
		case OBJECT_CONVERSION:
			tag = "ALTER CONVERSION";
			break;
		case OBJECT_DATABASE:
			tag = "ALTER DATABASE";
			break;
		case OBJECT_DOMAIN:
			tag = "ALTER DOMAIN";
			break;
		case OBJECT_EXTENSION:
			tag = "ALTER EXTENSION";
			break;
		case OBJECT_FDW:
			tag = "ALTER FOREIGN DATA WRAPPER";
			break;
		case OBJECT_FOREIGN_SERVER:
			tag = "ALTER SERVER";
			break;
		case OBJECT_FOREIGN_TABLE:
			tag = "ALTER FOREIGN TABLE";
			break;
		case OBJECT_FUNCTION:
			tag = "ALTER FUNCTION";
			break;
		case OBJECT_INDEX:
			tag = "ALTER INDEX";
			break;
		case OBJECT_LANGUAGE:
			tag = "ALTER LANGUAGE";
			break;
		case OBJECT_LARGEOBJECT:
			tag = "ALTER LARGE OBJECT";
			break;
		case OBJECT_OPCLASS:
			tag = "ALTER OPERATOR CLASS";
			break;
		case OBJECT_OPERATOR:
			tag = "ALTER OPERATOR";
			break;
		case OBJECT_OPFAMILY:
			tag = "ALTER OPERATOR FAMILY";
			break;
		case OBJECT_ROLE:
			tag = "ALTER ROLE";
			break;
		case OBJECT_RULE:
			tag = "ALTER RULE";
			break;
		case OBJECT_SCHEMA:
			tag = "ALTER SCHEMA";
			break;
		case OBJECT_SEQUENCE:
			tag = "ALTER SEQUENCE";
			break;
		case OBJECT_TABLE:
			tag = "ALTER TABLE";
			break;
		case OBJECT_TABLESPACE:
			tag = "ALTER TABLESPACE";
			break;
		case OBJECT_TRIGGER:
			tag = "ALTER TRIGGER";
			break;
		case OBJECT_TSCONFIGURATION:
			tag = "ALTER TEXT SEARCH CONFIGURATION";
			break;
		case OBJECT_TSDICTIONARY:
			tag = "ALTER TEXT SEARCH DICTIONARY";
			break;
		case OBJECT_TSPARSER:
			tag = "ALTER TEXT SEARCH PARSER";
			break;
		case OBJECT_TSTEMPLATE:
			tag = "ALTER TEXT SEARCH TEMPLATE";
			break;
		case OBJECT_TYPE:
			tag = "ALTER TYPE";
			break;
		case OBJECT_VIEW:
			tag = "ALTER VIEW";
			break;
		default:
			tag = "???";
			break;
	}

	return tag;
}

/*
 * CreateCommandTag
 *		utility to get a string representation of the command operation,
 *		given either a raw (un-analyzed) parsetree or a planned query.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 *
 * NB: all result strings must be shorter than COMPLETION_TAG_BUFSIZE.
 * Also, the result must point at a true constant (permanent storage).
 */
const char *
CreateCommandTag(Node *parsetree)
{
	const char *tag;

	switch (nodeTag(parsetree))
	{
			/* raw plannable queries */
		case T_InsertStmt:
			tag = "INSERT";
			break;

		case T_DeleteStmt:
			tag = "DELETE";
			break;

		case T_UpdateStmt:
			tag = "UPDATE";
			break;

		case T_SelectStmt:
			tag = "SELECT";
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
						tag = "BEGIN";
						break;

					case TRANS_STMT_START:
						tag = "START TRANSACTION";
						break;

					case TRANS_STMT_COMMIT:
						tag = "COMMIT";
						break;

					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_ROLLBACK_TO:
						tag = "ROLLBACK";
						break;

					case TRANS_STMT_SAVEPOINT:
						tag = "SAVEPOINT";
						break;

					case TRANS_STMT_RELEASE:
						tag = "RELEASE";
						break;

					case TRANS_STMT_PREPARE:
						tag = "PREPARE TRANSACTION";
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						tag = "COMMIT PREPARED";
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						tag = "ROLLBACK PREPARED";
						break;

					default:
						tag = "???";
						break;
				}
			}
			break;

		case T_DeclareCursorStmt:
			tag = "DECLARE CURSOR";
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				if (stmt->portalname == NULL)
					tag = "CLOSE CURSOR ALL";
				else
					tag = "CLOSE CURSOR";
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;

				tag = (stmt->ismove) ? "MOVE" : "FETCH";
			}
			break;

		case T_CreateDomainStmt:
			tag = "CREATE DOMAIN";
			break;

		case T_CreateSchemaStmt:
			tag = "CREATE SCHEMA";
			break;

		case T_CreateStmt:
			tag = "CREATE TABLE";
			break;
			
		// NEW FOR RECATHON
		case T_CreateRStmt:
			tag = "CREATE RECOMMENDER";
			break;

		case T_DropRecStmt:
			tag = "DROP RECOMMENDER";
			break;

		case T_CreateTableSpaceStmt:
			tag = "CREATE TABLESPACE";
			break;

		case T_DropTableSpaceStmt:
			tag = "DROP TABLESPACE";
			break;

		case T_AlterTableSpaceOptionsStmt:
			tag = "ALTER TABLESPACE";
			break;

		case T_CreateExtensionStmt:
			tag = "CREATE EXTENSION";
			break;

		case T_AlterExtensionStmt:
			tag = "ALTER EXTENSION";
			break;

		case T_AlterExtensionContentsStmt:
			tag = "ALTER EXTENSION";
			break;

		case T_CreateFdwStmt:
			tag = "CREATE FOREIGN DATA WRAPPER";
			break;

		case T_AlterFdwStmt:
			tag = "ALTER FOREIGN DATA WRAPPER";
			break;

		case T_CreateForeignServerStmt:
			tag = "CREATE SERVER";
			break;

		case T_AlterForeignServerStmt:
			tag = "ALTER SERVER";
			break;

		case T_CreateUserMappingStmt:
			tag = "CREATE USER MAPPING";
			break;

		case T_AlterUserMappingStmt:
			tag = "ALTER USER MAPPING";
			break;

		case T_DropUserMappingStmt:
			tag = "DROP USER MAPPING";
			break;

		case T_CreateForeignTableStmt:
			tag = "CREATE FOREIGN TABLE";
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case OBJECT_TABLE:
					tag = "DROP TABLE";
					break;
				case OBJECT_SEQUENCE:
					tag = "DROP SEQUENCE";
					break;
				case OBJECT_VIEW:
					tag = "DROP VIEW";
					break;
				case OBJECT_INDEX:
					tag = "DROP INDEX";
					break;
				case OBJECT_TYPE:
					tag = "DROP TYPE";
					break;
				case OBJECT_DOMAIN:
					tag = "DROP DOMAIN";
					break;
				case OBJECT_COLLATION:
					tag = "DROP COLLATION";
					break;
				case OBJECT_CONVERSION:
					tag = "DROP CONVERSION";
					break;
				case OBJECT_SCHEMA:
					tag = "DROP SCHEMA";
					break;
				case OBJECT_TSPARSER:
					tag = "DROP TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "DROP TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "DROP TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "DROP TEXT SEARCH CONFIGURATION";
					break;
				case OBJECT_FOREIGN_TABLE:
					tag = "DROP FOREIGN TABLE";
					break;
				case OBJECT_EXTENSION:
					tag = "DROP EXTENSION";
					break;
				case OBJECT_FUNCTION:
					tag = "DROP FUNCTION";
					break;
				case OBJECT_AGGREGATE:
					tag = "DROP AGGREGATE";
					break;
				case OBJECT_OPERATOR:
					tag = "DROP OPERATOR";
					break;
				case OBJECT_LANGUAGE:
					tag = "DROP LANGUAGE";
					break;
				case OBJECT_CAST:
					tag = "DROP CAST";
					break;
				case OBJECT_TRIGGER:
					tag = "DROP TRIGGER";
					break;
				case OBJECT_RULE:
					tag = "DROP RULE";
					break;
				case OBJECT_FDW:
					tag = "DROP FOREIGN DATA WRAPPER";
					break;
				case OBJECT_FOREIGN_SERVER:
					tag = "DROP SERVER";
					break;
				case OBJECT_OPCLASS:
					tag = "DROP OPERATOR CLASS";
					break;
				case OBJECT_OPFAMILY:
					tag = "DROP OPERATOR FAMILY";
					break;
				default:
					tag = "???";
			}
			break;

		case T_TruncateStmt:
			tag = "TRUNCATE TABLE";
			break;

		case T_CommentStmt:
			tag = "COMMENT";
			break;

		case T_SecLabelStmt:
			tag = "SECURITY LABEL";
			break;

		case T_CopyStmt:
			tag = "COPY";
			break;

		case T_RenameStmt:
			tag = AlterObjectTypeCommandTag(((RenameStmt *) parsetree)->renameType);
			break;

		case T_AlterObjectSchemaStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectSchemaStmt *) parsetree)->objectType);
			break;

		case T_AlterOwnerStmt:
			tag = AlterObjectTypeCommandTag(((AlterOwnerStmt *) parsetree)->objectType);
			break;

		case T_AlterTableStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableStmt *) parsetree)->relkind);
			break;

		case T_AlterDomainStmt:
			tag = "ALTER DOMAIN";
			break;

		case T_AlterFunctionStmt:
			tag = "ALTER FUNCTION";
			break;

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				tag = (stmt->is_grant) ? "GRANT" : "REVOKE";
			}
			break;

		case T_GrantRoleStmt:
			{
				GrantRoleStmt *stmt = (GrantRoleStmt *) parsetree;

				tag = (stmt->is_grant) ? "GRANT ROLE" : "REVOKE ROLE";
			}
			break;

		case T_AlterDefaultPrivilegesStmt:
			tag = "ALTER DEFAULT PRIVILEGES";
			break;

		case T_DefineStmt:
			switch (((DefineStmt *) parsetree)->kind)
			{
				case OBJECT_AGGREGATE:
					tag = "CREATE AGGREGATE";
					break;
				case OBJECT_OPERATOR:
					tag = "CREATE OPERATOR";
					break;
				case OBJECT_TYPE:
					tag = "CREATE TYPE";
					break;
				case OBJECT_TSPARSER:
					tag = "CREATE TEXT SEARCH PARSER";
					break;
				case OBJECT_TSDICTIONARY:
					tag = "CREATE TEXT SEARCH DICTIONARY";
					break;
				case OBJECT_TSTEMPLATE:
					tag = "CREATE TEXT SEARCH TEMPLATE";
					break;
				case OBJECT_TSCONFIGURATION:
					tag = "CREATE TEXT SEARCH CONFIGURATION";
					break;
				case OBJECT_COLLATION:
					tag = "CREATE COLLATION";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CompositeTypeStmt:
			tag = "CREATE TYPE";
			break;

		case T_CreateEnumStmt:
			tag = "CREATE TYPE";
			break;

		case T_CreateRangeStmt:
			tag = "CREATE TYPE";
			break;

		case T_AlterEnumStmt:
			tag = "ALTER TYPE";
			break;

		case T_ViewStmt:
			tag = "CREATE VIEW";
			break;

		case T_CreateFunctionStmt:
			tag = "CREATE FUNCTION";
			break;

		case T_IndexStmt:
			tag = "CREATE INDEX";
			break;

		case T_RuleStmt:
			tag = "CREATE RULE";
			break;

		case T_CreateSeqStmt:
			tag = "CREATE SEQUENCE";
			break;

		case T_AlterSeqStmt:
			tag = "ALTER SEQUENCE";
			break;

		case T_DoStmt:
			tag = "DO";
			break;

		case T_CreatedbStmt:
			tag = "CREATE DATABASE";
			break;

		case T_AlterDatabaseStmt:
			tag = "ALTER DATABASE";
			break;

		case T_AlterDatabaseSetStmt:
			tag = "ALTER DATABASE";
			break;

		case T_DropdbStmt:
			tag = "DROP DATABASE";
			break;

		case T_NotifyStmt:
			tag = "NOTIFY";
			break;

		case T_ListenStmt:
			tag = "LISTEN";
			break;

		case T_UnlistenStmt:
			tag = "UNLISTEN";
			break;

		case T_LoadStmt:
			tag = "LOAD";
			break;

		case T_ClusterStmt:
			tag = "CLUSTER";
			break;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->options & VACOPT_VACUUM)
				tag = "VACUUM";
			else
				tag = "ANALYZE";
			break;

		case T_ExplainStmt:
			tag = "EXPLAIN";
			break;

		case T_CreateTableAsStmt:
			if (((CreateTableAsStmt *) parsetree)->is_select_into)
				tag = "SELECT INTO";
			else
				tag = "CREATE TABLE AS";
			break;

		case T_VariableSetStmt:
			switch (((VariableSetStmt *) parsetree)->kind)
			{
				case VAR_SET_VALUE:
				case VAR_SET_CURRENT:
				case VAR_SET_DEFAULT:
				case VAR_SET_MULTI:
					tag = "SET";
					break;
				case VAR_RESET:
				case VAR_RESET_ALL:
					tag = "RESET";
					break;
				default:
					tag = "???";
			}
			break;

		case T_VariableShowStmt:
			tag = "SHOW";
			break;

		case T_DiscardStmt:
			switch (((DiscardStmt *) parsetree)->target)
			{
				case DISCARD_ALL:
					tag = "DISCARD ALL";
					break;
				case DISCARD_PLANS:
					tag = "DISCARD PLANS";
					break;
				case DISCARD_TEMP:
					tag = "DISCARD TEMP";
					break;
				default:
					tag = "???";
			}
			break;

		case T_CreateTrigStmt:
			tag = "CREATE TRIGGER";
			break;

		case T_CreatePLangStmt:
			tag = "CREATE LANGUAGE";
			break;

		case T_CreateRoleStmt:
			tag = "CREATE ROLE";
			break;

		case T_AlterRoleStmt:
			tag = "ALTER ROLE";
			break;

		case T_AlterRoleSetStmt:
			tag = "ALTER ROLE";
			break;

		case T_DropRoleStmt:
			tag = "DROP ROLE";
			break;

		case T_DropOwnedStmt:
			tag = "DROP OWNED";
			break;

		case T_ReassignOwnedStmt:
			tag = "REASSIGN OWNED";
			break;

		case T_LockStmt:
			tag = "LOCK TABLE";
			break;

		case T_ConstraintsSetStmt:
			tag = "SET CONSTRAINTS";
			break;

		case T_CheckPointStmt:
			tag = "CHECKPOINT";
			break;

		case T_ReindexStmt:
			tag = "REINDEX";
			break;

		case T_CreateConversionStmt:
			tag = "CREATE CONVERSION";
			break;

		case T_CreateCastStmt:
			tag = "CREATE CAST";
			break;

		case T_CreateOpClassStmt:
			tag = "CREATE OPERATOR CLASS";
			break;

		case T_CreateOpFamilyStmt:
			tag = "CREATE OPERATOR FAMILY";
			break;

		case T_AlterOpFamilyStmt:
			tag = "ALTER OPERATOR FAMILY";
			break;

		case T_AlterTSDictionaryStmt:
			tag = "ALTER TEXT SEARCH DICTIONARY";
			break;

		case T_AlterTSConfigurationStmt:
			tag = "ALTER TEXT SEARCH CONFIGURATION";
			break;

		case T_PrepareStmt:
			tag = "PREPARE";
			break;

		case T_ExecuteStmt:
			tag = "EXECUTE";
			break;

		case T_DeallocateStmt:
			{
				DeallocateStmt *stmt = (DeallocateStmt *) parsetree;

				if (stmt->name == NULL)
					tag = "DEALLOCATE ALL";
				else
					tag = "DEALLOCATE";
			}
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (stmt->utilityStmt != NULL)
						{
							Assert(IsA(stmt->utilityStmt, DeclareCursorStmt));
							tag = "DECLARE CURSOR";
						}
						else if (stmt->rowMarks != NIL)
						{
							/* not 100% but probably close enough */
							if (((PlanRowMark *) linitial(stmt->rowMarks))->markType == ROW_MARK_EXCLUSIVE)
								tag = "SELECT FOR UPDATE";
							else
								tag = "SELECT FOR SHARE";
						}
						else
							tag = "SELECT";
						break;
					case CMD_UPDATE:
						tag = "UPDATE";
						break;
					case CMD_INSERT:
						tag = "INSERT";
						break;
					case CMD_DELETE:
						tag = "DELETE";
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = "???";
						break;
				}
			}
			break;

			/* parsed-and-rewritten-but-not-planned queries */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (stmt->utilityStmt != NULL)
						{
							Assert(IsA(stmt->utilityStmt, DeclareCursorStmt));
							tag = "DECLARE CURSOR";
						}
						else if (stmt->rowMarks != NIL)
						{
							/* not 100% but probably close enough */
							if (((RowMarkClause *) linitial(stmt->rowMarks))->forUpdate)
								tag = "SELECT FOR UPDATE";
							else
								tag = "SELECT FOR SHARE";
						}
						else
							tag = "SELECT";
						break;
					case CMD_UPDATE:
						tag = "UPDATE";
						break;
					case CMD_INSERT:
						tag = "INSERT";
						break;
					case CMD_DELETE:
						tag = "DELETE";
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(stmt->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = "???";
						break;
				}
			}
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			tag = "???";
			break;
	}

	return tag;
}


/*
 * GetCommandLogLevel
 *		utility to get the minimum log_statement level for a command,
 *		given either a raw (un-analyzed) parsetree or a planned query.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 */
LogStmtLevel
GetCommandLogLevel(Node *parsetree)
{
	LogStmtLevel lev;

	switch (nodeTag(parsetree))
	{
			/* raw plannable queries */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_SelectStmt:
			if (((SelectStmt *) parsetree)->intoClause)
				lev = LOGSTMT_DDL;		/* SELECT INTO */
			else
				lev = LOGSTMT_ALL;
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DeclareCursorStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClosePortalStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_FetchStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStmt:
		case T_CreateRStmt:
		case T_DropRecStmt:
		case T_CreateForeignTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
		case T_AlterTableSpaceOptionsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_AlterUserMappingStmt:
		case T_DropUserMappingStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_TruncateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_CommentStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_SecLabelStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CopyStmt:
			if (((CopyStmt *) parsetree)->is_from)
				lev = LOGSTMT_MOD;
			else
				lev = LOGSTMT_ALL;
			break;

		case T_PrepareStmt:
			{
				PrepareStmt *stmt = (PrepareStmt *) parsetree;

				/* Look through a PREPARE to the contained stmt */
				lev = GetCommandLogLevel(stmt->query);
			}
			break;

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *ps;

				/* Look through an EXECUTE to the referenced stmt */
				ps = FetchPreparedStatement(stmt->name, false);
				if (ps)
					lev = GetCommandLogLevel(ps->plansource->raw_parse_tree);
				else
					lev = LOGSTMT_ALL;
			}
			break;

		case T_DeallocateStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_RenameStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOwnerStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDefaultPrivilegesStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DefineStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CompositeTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRangeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_IndexStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RuleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DoStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreatedbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropdbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_NotifyStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ListenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_UnlistenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_LoadStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClusterStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VacuumStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *stmt = (ExplainStmt *) parsetree;
				bool		analyze = false;
				ListCell   *lc;

				/* Look through an EXPLAIN ANALYZE to the contained stmt */
				foreach(lc, stmt->options)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if (strcmp(opt->defname, "analyze") == 0)
						analyze = defGetBoolean(opt);
					/* don't "break", as explain.c will use the last value */
				}
				if (analyze)
					return GetCommandLogLevel(stmt->query);

				/* Plain EXPLAIN isn't so interesting */
				lev = LOGSTMT_ALL;
			}
			break;

		case T_CreateTableAsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VariableSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_VariableShowStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DiscardStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePLangStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ReassignOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_LockStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ConstraintsSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CheckPointStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ReindexStmt:
			lev = LOGSTMT_ALL;	/* should this be DDL? */
			break;

		case T_CreateConversionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateCastStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpClassStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSDictionaryStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSConfigurationStmt:
			lev = LOGSTMT_DDL;
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
						lev = LOGSTMT_MOD;
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

			/* parsed-and-rewritten-but-not-planned queries */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}

			}
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			lev = LOGSTMT_ALL;
			break;
	}

	return lev;
}
