#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#define AR_DEC 10
#define ANSLINE_WIDTH 512
#define AR_RBUF_WIDTH 1024
#define AR_RND_STATESIZE 512
#define AR_MIN(X, Y)  ((X) < (Y) ? (X) : (Y))
#define AR_FABS(X) ((X) < 0 ? ((X) * -1) : (X))

/* structure to store problem results */
struct probans {
    long double score;
    int class;
};

/* worker thread args */
struct thread_args {
    struct probans *ans_a;
    struct probans *ans_b;
    unsigned int num_probs_a;
    unsigned int num_probs_b;
    int tclass;
    unsigned int R;
    unsigned int seed;
    unsigned int ui_update_speed;
    unsigned int rcode;
};

/* string to int */
int ar_strtoint(char *str) {
    int class = 0;
    int c;
    for (c = 0; (c < ANSLINE_WIDTH) && (str[c] != '\0'); c++) {
        class ^= str[c] << ((c * CHAR_BIT) % (sizeof(class) * CHAR_BIT));
    }
    return class;
}

/* convert a string into a long int */
unsigned int ar_strtoui(char *s) {
    unsigned long int tmp = strtoul(s, NULL, AR_DEC);
    unsigned int ans;
    if (tmp <= UINT_MAX) {
        ans = tmp;
    } else {
        ans = UINT_MAX;
    }
    return ans;
}

/* print settings */
void ar_psettings(char *file_a,
                  char *file_b,
                  char *tclass_str,
                  int tclass,
                  unsigned int seed,
                  long int R,
                  int shuffle,
                  unsigned int jobs) {
    fprintf(stderr, "settings:\n");
    fprintf(stderr, "   input for method A : %s\n", file_a);
    fprintf(stderr, "   input for method B : %s\n", file_b);
    fprintf(stderr, "   target class       : %s (intrnl. rep. %d)\n",
                    tclass_str,
                    tclass);
    fprintf(stderr, "   initial random seed: %u\n", seed);
    fprintf(stderr, "   total AR rounds    : %ld\n", R);
    fprintf(stderr, "   shuffle lists      : %d\n", shuffle);
    fprintf(stderr, "   worker threads     : %d\n", jobs);
}

/* print help */
void ar_help(char *name) {
    fprintf(stderr, "%s -a FILEA -b FILEB [-t TCLASS] [-s SEED] [-R ROUNDS] [-x] [-j THRDS] [-h]\n", name);
    fprintf(stderr, "   FILEA   problem score and classes for method A.\n");
    fprintf(stderr, "   FILEB   problem score and classes for method B.\n");
    fprintf(stderr, "   TCLASS  target class for TPR/FPR calculation.\n");
    fprintf(stderr, "   SEED    initial random seed for AR.\n");
    fprintf(stderr, "   -x      whether to individually shuffle lists too.\n");
    fprintf(stderr, "   THRDS   total number of concurrent worker threads.\n");
    fprintf(stderr, "   -h      print this then exist.\n");
}

/* get options */
int ar_getopt(int argc,
              char **argv,
              char **file_a,
              char **file_b,
              char **tclass_str,
              int *tclass,
              unsigned int *seed,
              long int *R,
              int *shuffle,
              unsigned int *jobs,
              int *help) {
    /* get options */
    int opt;
    while ((opt = getopt(argc, argv, "a:b:t:s:R:xj:h")) != -1) {
        switch (opt) {
            case 'a':
                *file_a = optarg;
                break;
            case 'b':
                *file_b = optarg;
                break;
            case 't':
                *tclass_str = optarg;
                *tclass = ar_strtoint(optarg);
                break;
            case 's':
                *seed = ar_strtoui(optarg);
                break;
            case 'R':
                *R = strtol(optarg, NULL, AR_DEC);
                break;
            case 'x':
                *shuffle = 1;
                break;
            case 'j':
                *jobs = ar_strtoui(optarg);
                break;
            case 'h':
                *help = 1;
                break;
        }
    }

    /* simple syntax check */
    int rcode = 0;
    if (*jobs == 0) {
        fprintf(stderr, "error: jobs too small (%u).\n", *jobs);
        rcode = 1;
    }
    if ((*file_a == NULL) || (*file_b == NULL)) {
        rcode = 1;
    } else {
        if (access(*file_a, R_OK) == -1) {
            fprintf(stderr, "error: can't open file '%s' for read.\n", *file_a);
            rcode = 1;
        }
        if (access(*file_b, R_OK) == -1) {
            fprintf(stderr, "error: can't open file '%s' for read.\n", *file_b);
            rcode = 1;
        }
    }

    return rcode;
}

/* count number of problems in an input file
 * note: this function is ridiculously inefficient; rewrite when free */
unsigned int ar_num_probs(char *file) {
    unsigned int num_probs = 0;
    FILE *fh = fopen(file, "r");
    char rbuf[AR_RBUF_WIDTH];
    size_t readc = 0;
    while ((readc = fread(rbuf, sizeof(char), AR_RBUF_WIDTH, fh))) {
        size_t c;
        for (c = 0; c < readc; c++) {
            if (rbuf[c] == '\n') {
                num_probs++;
            }
        }
    }
    fclose(fh);
    return num_probs;
}

/* allocate memory for problem answers */
void ar_malloc(size_t size, struct probans **ans, char *method) {
    *ans = malloc(size);
    if (*ans == NULL) { 
        fprintf(stderr, "error: failed to malloc to load answers for %s.\n", method);
        exit(1);
    }
}

/* load answer files */
void ar_load(char *file, struct probans *ans, int tclass, char *method) {
    /* open file */
    fprintf(stderr, "loading answers of %s.. ", method);
    FILE *fh = fopen(file, "r");

    /* load score/class into ans */;
    unsigned int ansline_id = 0;
    char rbuf[AR_RBUF_WIDTH];
    size_t readc = 0;
    char ansline[ANSLINE_WIDTH];
    unsigned int ansline_i = 0;
    int tclass_found = 0;
    while ((readc = fread(rbuf, sizeof(char), AR_RBUF_WIDTH, fh))) {
        size_t c;
        for (c = 0; c < readc; c++) {
            /* read answer line */
            if (ansline_i < ANSLINE_WIDTH) {
                ansline[ansline_i] = rbuf[c];
                ansline_i++;
            } else {
                fprintf(stderr, "error: too wide answer (> %u chars) at line %u in"
                " '%s'", ansline_i, ansline_id + 1, file);
                exit(1);
            }

            /* process read answer line */
            if (rbuf[c] == '\n') {
                /* finalize current line */
                ansline[ansline_i] = '\0';
                ansline_i = 0;

                /* extract score and class from t */
                long double score;
                char class_tmp[ANSLINE_WIDTH];
                sscanf(ansline, "%Lf %s", &score, class_tmp);
                int class = ar_strtoint(class_tmp);

                /* store it in answers array */
                ans[ansline_id].score = score;
                ans[ansline_id].class = class;

                /* some fool-proof stats */
                if (class == tclass) tclass_found = 1;

                /* be ready for next line (if any) */
                ansline_id++;
            }
        }
    }

    /* some sanity check */
    if (tclass_found == 0) {
        fprintf(stderr, "error: target class (%d) not found in '%s'.\n",
                tclass, file);
        exit(1);
    }

    /* free resources */
    fclose(fh);
    fprintf(stderr, "ok (%d answers)\n", ansline_id);
}

/* test answers by given threshold */
void ar_test(struct probans *ans,
             unsigned int num_probs,
             int tclass,
             long double t,
             long double *tpr,
             long double *fpr) {
    /* counters */
    unsigned int tps = 0;
    unsigned int fps = 0;
    unsigned int tns = 0;
    unsigned int fns = 0;

    /* test answers with given threshold */
    unsigned int c;
    for (c = 0; c < num_probs; c++) {
        if (ans[c].class == tclass) { /* is TCLASS */
            if (ans[c].score > t) { /* predicted TCLASS */
                tps++;
            } else { /* predicted NOT TCLASS */
                fns++;
            }
        } else { /* is NOT TCLASS */
            if (ans[c].score > t) { /* predicted TCLASS */
                fps++;
            } else { /* predicted NOT TCLASS */
                tns++;
            }
        }
    }

    /* calculate rates */
    *tpr = (long double)tps / (tps + fns);
    *fpr = (long double)fps / (fps + tns);
}

/* auc calculator */
long double ar_auc(struct probans *ans, unsigned int num_probs, int tclass) {
    long double auc = 0;
    long double t;
    long double fpr_last = 0;
    for (t = 1.01; t >= -0.01; t -= 0.001) {
        /* calculate tpr/fpr rates */
        long double tpr, fpr;
        ar_test(ans, num_probs, tclass, t, &tpr, &fpr);

        /* calculate area under column */
        long double auc_column = AR_FABS(fpr_last - fpr) * tpr;
        auc += auc_column;

        /* update fpr_last */
        fpr_last = fpr;

    }
    return auc;
}

/* AR worker thread */
void *ar_worker(void *targsin) {
    struct thread_args *targs = targsin;

    /* initialize random seed */
    struct random_data randbuf;
    char statebuf[AR_RND_STATESIZE];
    memset(&randbuf, 0, sizeof(struct random_data));
    if (initstate_r(targs->seed, statebuf, sizeof(char) * AR_RND_STATESIZE, &randbuf) == -1) {
        perror("INITSTATE_R");
        fprintf(stderr, "error: failed to init seed.\n");
        exit(1);
    }

    /* total number of times AUC(randomized results) >= AUC(evaluated) */
    unsigned int times_null_true = 0;

    /* calculate auc with evaluated problem answers */
    long double auc_a = ar_auc(targs->ans_a, AR_MIN(targs->num_probs_a, targs->num_probs_b), targs->tclass);
    long double auc_b = ar_auc(targs->ans_b, AR_MIN(targs->num_probs_a, targs->num_probs_b), targs->tclass);

    /* initialize randomized answers array */
    struct probans *ans_x;
    struct probans *ans_y;
    ar_malloc(sizeof(struct probans) * targs->num_probs_a, &ans_x, "X");
    ar_malloc(sizeof(struct probans) * targs->num_probs_b, &ans_y, "Y");
    memcpy(ans_x, targs->ans_a, sizeof(struct probans) * targs->num_probs_a);
    memcpy(ans_y, targs->ans_b, sizeof(struct probans) * targs->num_probs_b);

    /* UI feedback */
    unsigned int ui = 0;

    /* repeat simulation for R times */
    unsigned int r;
    int32_t rresult;
    for (r = 0; r < (targs->R); r++) {
        /* simulate random answers */
        unsigned int i;
        for (i = 0; i < AR_MIN(targs->num_probs_a, targs->num_probs_b); i++) {
            if (random_r(&randbuf, &rresult) == -1) {
                perror("RANDOM_R");
                fprintf(stderr, "error: random_r failed.\n");
                exit(1);
            }
            if ((rresult % 2) == 0) {
                struct probans tmp;
                tmp.score = ans_x[i].score;
                tmp.class = ans_x[i].class;
                ans_x[i].score = ans_y[i].score;
                ans_x[i].class = ans_y[i].class;
                ans_y[i].score = tmp.score;
                ans_y[i].class = tmp.class;
            }
        }

        /* calculate auc with randomized problem answers */
        long double auc_x = ar_auc(ans_x, AR_MIN(targs->num_probs_a, targs->num_probs_b), targs->tclass);
        long double auc_y = ar_auc(ans_y, AR_MIN(targs->num_probs_a, targs->num_probs_b), targs->tclass);

        /* compare difference between evaluated and randomized */
        if (AR_FABS(auc_x - auc_y) >= AR_FABS(auc_a - auc_b)) {
            times_null_true++;
        }

        /* update UI */
        if (++ui % targs->ui_update_speed  == 0) fprintf(stderr, ".");
    }

    /* free resources */
    free(ans_x);
    free(ans_y);

    /* return code */
    targs->rcode = times_null_true;
    return NULL;
}


int main(int argc, char **argv) {
    /* initialize options */
    char *file_a = NULL;
    char *file_b = NULL;
    char *tclass_str = NULL;
    int tclass = 0;
    unsigned int seed = 0;
    long int R = 1000;
    int shuffle = 0; /* whether to shuffle lists too */
    unsigned int jobs = 4;
    int help = 0; /* whether to shuffle lists too */

    /* get options */
    if (ar_getopt(argc, argv, &file_a, &file_b, &tclass_str,
                  &tclass, &seed, &R, &shuffle, &jobs,
                  &help) || help) {
        ar_help(argv[0]);
        return 1;
    }

    /* ensure rounds are never less than asked (but possibly slightly more;
     * simplifies code) */
    R = ceil((double)R / jobs) * jobs;

    /* print settings */
    ar_psettings(file_a, file_b, tclass_str, tclass, seed, R, shuffle, jobs);

    /* count number of problems in file_a */
    fprintf(stderr, "counting problems..");
    unsigned int num_probs_a = ar_num_probs(file_a);
    unsigned int num_probs_b = ar_num_probs(file_b);
    fprintf(stderr, " ok (%u in A, %u in B)\n", num_probs_a, num_probs_b);

    /* allocate memory to store problem answers */
    fprintf(stderr, "allocating memory to load problems..");
    struct probans *ans_a, *ans_b;
    ar_malloc((sizeof(struct probans) * num_probs_a), &ans_a, "A");
    ar_malloc((sizeof(struct probans) * num_probs_b), &ans_b, "B");
    fprintf(stderr, " ok\n");

    /* load problem answers */
    ar_load(file_a, ans_a, tclass, "A");
    ar_load(file_b, ans_b, tclass, "B");

    /* calculate AUC of ROC of answers */
    fprintf(stderr, "calculaging AUC of A..");
    long double auc_a = ar_auc(ans_a, AR_MIN(num_probs_a, num_probs_b), tclass);
    fprintf(stderr, " ok (%Lf)\n", auc_a);
    fprintf(stderr, "calculaging AUC of B..");
    long double auc_b = ar_auc(ans_b, AR_MIN(num_probs_a, num_probs_b), tclass);
    fprintf(stderr, " ok (%Lf)\n", auc_b);

    /* run AR worker threads */
    struct thread_args targs[jobs];
    unsigned int times_null_true = 0;
    pthread_t threads[jobs];
    fprintf(stderr, "running AR workers..");
    unsigned int c;
    for (c = 0; c < jobs; c++) {
        targs[c].ans_a = ans_a;
        targs[c].ans_b = ans_b;
        targs[c].num_probs_a = num_probs_a;
        targs[c].num_probs_b = num_probs_b;
        targs[c].tclass = tclass;
        targs[c].R = R / jobs;
        targs[c].seed = seed + 5 + c;
        targs[c].ui_update_speed = R / jobs / (35 / jobs);
        if (pthread_create(&threads[c], NULL, &ar_worker, (void *)(&targs[c]))) {
            fprintf(stderr, "error: failed to start thread (%u).\n", c);
            return 1;
        }
    }
    for (c = 0; c < jobs; c++) {
        if (pthread_join(threads[c], NULL)) {
            fprintf(stderr, "error: failed to join thread (%u).\n", c);
            return 1;
        }
        times_null_true += targs[c].rcode;
    }
    long double p = (long double)times_null_true / R;
    fprintf(stderr, " ok (p = %Lf)\n", p);

    /* free resources */
    free(ans_a);
    free(ans_b);

    return 0;
}
