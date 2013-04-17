/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007 Tokutek Inc.  All rights reserved."
#ident "$Id$"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>

#include <toku_pthread.h>
#include <unistd.h>
#include <memory.h>
#include <sys/stat.h>
#include <db.h>

#include "threaded_stress_test_helpers.h"

//
// This test is a form of stress that does operations on a single dictionary:
// We create a dictionary bigger than the cachetable (around 4x greater).
// Then, we spawn a bunch of pthreads that do the following:
//  - scan dictionary forward with bulk fetch
//  - scan dictionary forward slowly
//  - scan dictionary backward with bulk fetch
//  - scan dictionary backward slowly
//  - update existing values in the dictionary with db->put(DB_YESOVERWRITE)
//  - do random point queries into the dictionary
// With the small cachetable, this should produce quite a bit of churn in reading in and evicting nodes.
// If the test runs to completion without crashing, we consider it a success.
//
// This test differs from stress2 in that it verifies the last value on an update.
//

static void
stress_table(DB_ENV *env, DB **dbp, struct cli_args *cli_args) {
    int n = cli_args->num_elements;

    //
    // the threads that we want:
    //   - one thread constantly updating random values
    //   - one thread doing table scan with bulk fetch
    //   - one thread doing table scan without bulk fetch
    //   - one thread doing random point queries
    //
    if (verbose) printf("starting creation of pthreads\n");
    const int num_threads = 4 + cli_args->num_update_threads + cli_args->num_ptquery_threads;
    struct arg myargs[num_threads];
    for (int i = 0; i < num_threads; i++) {
        arg_init(&myargs[i], n, dbp, env, cli_args);
    }

    // make the forward fast scanner
    myargs[0].fast = TRUE;
    myargs[0].fwd = TRUE;
    myargs[0].operation = scan_op_no_check;

    // make the forward slow scanner
    myargs[1].fast = FALSE;
    myargs[1].fwd = TRUE;
    myargs[1].operation = scan_op_no_check;

    // make the backward fast scanner
    myargs[2].fast = TRUE;
    myargs[2].fwd = FALSE;
    myargs[2].operation = scan_op_no_check;

    // make the backward slow scanner
    myargs[3].fast = FALSE;
    myargs[3].fwd = FALSE;
    myargs[3].operation = scan_op_no_check;

    // make the guy that updates the db
    for (int i = 4; i < 4 + cli_args->num_update_threads; ++i) {
        myargs[i].update_history_buffer = toku_xmalloc(n * (sizeof myargs[i].update_history_buffer[0]));
        memset(myargs[i].update_history_buffer, 0, n * (sizeof myargs[i].update_history_buffer[0]));
        myargs[i].operation = update_with_history_op;
    }

    // make the guys that do point queries
    for (int i = 4 + cli_args->num_update_threads; i < num_threads; i++) {
        myargs[i].operation = ptquery_op;
    }

    run_workers(myargs, num_threads, cli_args->time_of_test, false);

    for (int i = 4; i < 4 + cli_args->num_update_threads; ++i) {
        toku_free(myargs[i].update_history_buffer);
    }
}

int
test_main(int argc, char *const argv[]) {
    struct cli_args args = DEFAULT_ARGS;
    parse_stress_test_args(argc, argv, &args);
    stress_test_main(&args);
    return 0;
}
