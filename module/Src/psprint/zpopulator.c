/*
 * zpopulator – multi-threading support for Zsh
 *
 * Copyright (c) 2017 Sebastian Gniazdowski
 * All rights reserved.
 *
 * Licensed under MIT, GPLv3.
 */

/* Make-things-compile stuff {{{ */

#include "../../config.h"

#define MAXDEPTH 7

#ifdef ZSH_HASH_DEBUG
# define HASHTABLE_DEBUG_MEMBERS \
    /* Members of struct hashtable used for debugging hash tables */ \
    HashTable next, last;	/* linked list of all hash tables           */ \
    char *tablename;		/* string containing name of the hash table */ \
    PrintTableStats printinfo;	/* pointer to function to print table stats */
#else /* !ZSH_HASH_DEBUG */
# define HASHTABLE_DEBUG_MEMBERS
#endif /* !ZSH_HASH_DEBUG */

#define HASHTABLE_INTERNAL_MEMBERS \
    ScanStatus scan;		/* status of a scan over this hashtable     */ \
    HASHTABLE_DEBUG_MEMBERS

#include "zpopulator.mdh"
#include "zpopulator.pro"

#include <unistd.h>
#include <pthread.h>

static HashTable my_newparamtable(int size, char const *name);
static HashTable my_newhashtable(int size, UNUSED(char const *name), UNUSED(PrintTableStats printinfo));
static void my_emptyhashtable(HashTable ht);
static void my_resizehashtable(HashTable ht, int newsize);
static void my_addhashnode(HashTable ht, char *nam, void *nodeptr);
static HashNode my_addhashnode2(HashTable ht, char *nam, void *nodeptr);
static void my_expandhashtable(HashTable ht);
static HashNode my_getparamnode(HashTable ht, const char *nam);
static HashNode my_gethashnode2(HashTable ht, const char *nam);
static HashNode my_removehashnode(HashTable ht, const char *nam);
static void my_freeparamnode(HashNode hn);
static void my_printhashtabinfo(HashTable ht);
static void * my_zshcalloc(size_t size);
static void * my_zalloc(size_t size);
static void my_zfree(void *p, UNUSED(int sz));
static void my_zsfree(char *p);
static void * my_zrealloc(void *ptr, size_t size);
static char * my_ztrdup(const char *s);

static void my_assigngetset(Param pm);
static char * my_strgetfn(Param pm);
static void my_strsetfn(Param pm, char *x);
static void my_stdunsetfn(Param pm, UNUSED(int exp));

/* }}} */

#define OUTPUT_ARRAY 1
#define OUTPUT_HASH 2
#define OUTPUT_VARS 3

#define WORKER_COUNT 32

#define ROINTPARAMDEF(name, var) \
    { name, PM_INTEGER | PM_READONLY, (void *) var, NULL,  NULL, NULL, NULL }

#define ROARRPARAMDEF(name, var) \
    { name, PM_ARRAY | PM_READONLY, (void *) var, NULL,  NULL, NULL, NULL }

struct outconf {
    int id;
    int mode;
    char *target;
    Param target_pm;
    char *main_d;
    int main_d_len;
    char *sub_d;
    int sub_d_len;
    FILE *stream;
    FILE *err;
    FILE *r_devnull;
    int silent;
    int only_global;
    int debug;
};

struct zpinconf {
    char *command[2];
    FILE *w_devnull;
};

/* pthread data structures for all threads */
pthread_t workers[ WORKER_COUNT ];

/* Holds WORKER_COUNT designators of worker activity */
char **worker_finished;

/* Holds number of workers being active */
int workers_count = 0;

static
Param ensurethereishash( char *name, struct outconf *oconf ) {
    Param pm;

    pm = (Param) paramtab->getnode( paramtab, name );
    if ( ! pm ) {
        pm = createparam( name, PM_HASHED );
        if ( ! pm ) {
            return NULL;
        }
    } else {
        if ( oconf->only_global && pm->level != 0 ) {
            if ( ! oconf->silent ) {
                fprintf( stderr, "Non-global variable `%s' exists, aborting (-g)\n", name );
                fflush( stderr );
            }
            return NULL;
        }

        if ( ! ( pm->node.flags & PM_HASHED ) ) {
            if ( ! oconf->silent ) {
                fprintf( stderr, "Variable `%s' isn't hash table, aborting\n", name );
            }
            return NULL;
        }

        if ( oconf->debug ) {
            if ( pm ) {
                fprintf( stderr, "zpopulator: Reused parameter, level: %d, locallevel: %d, unset: %d, unsetfn: %p\n",
                        pm->level, locallevel, pm->node.flags & PM_UNSET ? 1 : 0, pm->gsu.s->unsetfn );
                if ( pm->old ) {
                    fprintf( stderr, "zpopulator: There is pm->old, level: %d\n", pm->old->level );
                }
                fflush( stderr );
            }
        }

        return pm;
    }

    if ( oconf->debug ) {
        if ( pm ) {
            fprintf( stderr, "zpopulator: Created parameter, level: %d, locallevel: %d, unset: %d, unsetfn: %p\n",
                    pm->level, locallevel, pm->node.flags & PM_UNSET ? 1 : 0, pm->gsu.s->unsetfn );
            if ( pm->old ) {
                fprintf( stderr, "zpopulator: There is pm->old, level: %d\n", pm->old->level );
            }
            fflush( stderr );
        }
    }

    /* This creates standard hash. However, it normally
     * had getparamnode() call in getnode field, with
     * PM_AUTOLOAD features - this is disabled here */
    pm->u.hash = my_newparamtable( 32, name );
    if ( ! pm->u.hash ) {
        paramtab->removenode( paramtab, name );
        paramtab->freenode( &pm->node );
        if ( ! oconf->silent ) {
            fprintf( stderr, "zpopulator: Out of memory when allocating hash\n" );
        }
    }

    return pm;
}

static
void set_in_hash( struct outconf *oconf, const char *key, const char *value ) {
    if ( NULL == key || key[0] == '\0' ) {
        return;
    }

    HashTable ht = (HashTable) oconf->target_pm->gsu.h->getfn( oconf->target_pm );
    if ( ! ht ) {
        if ( oconf->debug ) {
            fprintf( oconf->err, "zpopulator: Hash table `%s' is null\n", oconf->target );
            fflush( oconf->err );
        }
        return;
    }
    Param val_pm = (Param) ht->getnode( ht, key );

    /* Entry for key doesn't exist ? */
    if ( ! val_pm ) {
        val_pm = (Param) my_zshcalloc( sizeof (*val_pm) );
        val_pm->node.flags = PM_SCALAR | PM_HASHELEM;
	assigngetset(val_pm); // free of signal queueing

        my_strsetfn( val_pm, my_ztrdup(value) );
        ht->addnode( ht, my_ztrdup( key ), val_pm );
    } else {
        my_strsetfn( val_pm, my_ztrdup(value) );
    }
}

static void
show_help() {
    printf( "Usage: zpin \"source_program\" | zpopulator [-a name|-A name|-x] [-d string] [-D string] WORKER_ID\n");
    printf( "Options:\n" );
    printf( " -a name - put input into global array `name'\n" );
    printf( " -A name - put input into global hash `name', keys and values\n" );
    printf( "           alternating\n" );
    printf( " -x - put input into global variables, names and values determined\n" );
    printf( "      as with hash (-d/-D); variables must already exist\n" );
    printf( " -d string - main delimeter dividing into array elements (default: \"\\n\")\n" );
    printf( " -D string - sub-delimeter, to divide into key and value (default: \":\")\n" );
    printf( " -g - ensure that there are only global variables in use - saves\n" );
    printf( "      disappointments when learning that output variable must\n" );
    printf( "      continuously live during computation\n" );
    printf( " WORKER_ID - number of worker slot to use, 1..%d\n", WORKER_COUNT );
    fflush( stdout );
}

static
void free_oconf( struct outconf *oconf ) {
    if ( oconf ) {
        if ( NULL == oconf->stream || fileno( oconf->stream ) == -1 ) {
            int file = oconf->stream ? fileno( oconf->stream ) : 0;
            fprintf( oconf->err, "zpopulator: Input fail: %p (%d), %s\n", oconf->stream, file, strerror( errno ) );
            fflush( oconf->err );
        } else {
            if ( -1 != fcntl( fileno( oconf->stream ), F_GETFD ) ) {
                /* fclose: "dissociates the named stream from
                 * its underlying file or set of functions"
                 */
                if ( 0 != fclose( oconf->stream ) ) {
                    fprintf( oconf->err, "zpopulator: Warning: could not close input stream (%p): %s\n", oconf->stream, strerror( errno ) );
                    fflush( oconf->err );
                    /* TODO */
                }
            }
        }

        if ( -1 != fcntl( fileno( oconf->err ), F_GETFD ) ) {
            if ( 0 != fclose( oconf->err ) ) {
                /* TODO */
            }
        }

        if ( oconf->target ) {
            zsfree( oconf->target );
        }
        if ( oconf->main_d ) {
            zsfree( oconf->main_d );
        }
        if ( oconf->sub_d ) {
            zsfree( oconf->sub_d );
        }
        zfree( oconf, sizeof( struct outconf ) );
    }
}

static
void free_oconf_thread_safe( struct outconf *oconf ) {
    if ( oconf ) {
        if ( NULL == oconf->stream || fileno( oconf->stream ) == -1 ) {
            int file = oconf->stream ? fileno( oconf->stream ): 0;
            fprintf( oconf->err, "zpopulator: (thread) Input fail: %p (%d), %s\n", oconf->stream, file, strerror( errno ) );
            fflush( oconf->err );
        } else {
            if ( -1 != fcntl( fileno( oconf->stream ), F_GETFD ) ) {
                /* fclose: "dissociates the named stream from
                 * its underlying file or set of functions"
                 */
                if ( 0 != fclose( oconf->stream ) ) {
                    fprintf( oconf->err, "zpopulator: (thread) Warning: could not close input stream (%p): %s\n", oconf->stream, strerror( errno ) );
                    fflush( oconf->err );
                    /* TODO */
                }
            }
        }

        if ( -1 != fcntl( fileno( oconf->err ), F_GETFD ) ) {
            if ( 0 != fclose( oconf->err ) ) {
                /* TODO */
            }
        }

        if ( oconf->target ) {
            my_zsfree( oconf->target );
        }
        if ( oconf->main_d ) {
            my_zsfree( oconf->main_d );
        }
        if ( oconf->sub_d ) {
            my_zsfree( oconf->sub_d );
        }
        my_zfree( oconf, sizeof( struct outconf ) );
    }
}

/* this function is run by the second thread */
static
void *process_input( void *void_ptr ) {
    char *buf, *found;
    int bufsize = 256, index = 0;

    /* Instructs what to do */
    struct outconf *oconf = ( struct outconf *) void_ptr;

    buf = malloc( bufsize );
    if ( ! buf ) {
        if ( ! oconf->silent ) {
            fputs( "zpopulator: Out of memory in thread", oconf->err );
            fflush( oconf->err );
        }
        free_oconf_thread_safe( oconf );
        workers_count --;
        pthread_exit( NULL );
        return NULL;
    }

    /* Make those handy */
    int main_d_len = oconf->main_d_len;
    int sub_d_len = oconf->sub_d_len;
    int read_size = 5;
    volatile int loop_counter = 0;

    while ( 1 ) {
        /* Loop counter used for debugging */
        ++ loop_counter;

        /* Check if data will fit into buffer - sum current
         * number of bytes in buffer (`index`) and possible
         * maximum read size (`read_size`) and trailing null
         * byte that will be added. Also, possible addition
         * of main delimeter. */
        if ( index + read_size + 1 + main_d_len > bufsize ) {
            bufsize *= 1.5;
            char * save_buf = buf;
            buf = realloc( buf, bufsize );
            if ( ! buf ) {
                fprintf( oconf->err, "zpopulator: Fatal error - could not reallocate buffer, lines are too long" );
                fflush( oconf->err );
                free( save_buf );
                break;
            }
        }

        /* Read e.g. 5 characters, putting them after previous portion */
        int count = fread( buf + index, 1, read_size, oconf->stream );
        /* Ensure that our whole data is a string - null terminated */
        buf[ index + count ] = '\0';

        /* No data in buffer, and stream is ended -> break */
        if ( feof( oconf->stream ) && (index+count) == 0 ) {
            break;
        }

        /* Handle case with no final trailing main delimeter. This
         * test can be ran multiple times, and only for the final
         * portion of data the strstr() will return NULL. */
        if ( feof( oconf->stream ) ) {
            if ( oconf->debug ) {
                fprintf( oconf->err, "End of stream with unprocessed data, index: %d, buf: %s\n", index, buf );
                fflush( oconf->err );
            }
            if( ! strstr( buf, oconf->main_d ) ) {
                /* Enough room is ensured in top in this loop */
                strcat( buf, oconf->main_d );
            }
        }

        /* Look for main record divider */
        if ( ( found = strstr( buf, oconf->main_d ) ) ) {
            /**/
            /* Will have to split one more time if OUTPUT_HASH */
            /**/

            if ( oconf->mode == OUTPUT_HASH ) {
                /* Remember first character of main divider */
                char mbkp = found[ 0 ];
                /* Create string of the delimeted portion */
                found[ 0 ] = '\0';

                char *sfound = strstr( buf, oconf->sub_d );
                if ( ! sfound ) {
                    set_in_hash( oconf, buf, "" );
                } else {
                    char sbkp = sfound[ 0 ];
                    /* Create string of the left side */
                    sfound[ 0 ] = '\0';

                    /* Store left side as key, right side as data */
                    set_in_hash( oconf, buf, sfound + sub_d_len );

                    /* Be maximal sane, restore overwritten data */
                    sfound[ 0 ] = sbkp;
                }

                /* Be maximal sane, restore overwritten data */
                found[ 0 ] = mbkp;
            } else

            /**/
            /* Just store for OUTPUT_ARRAY */
            /**/

            if ( oconf->mode == OUTPUT_ARRAY ) {

            } else

            /**/
            /* Create variable for every key (string before sub-delimeter) */
            /**/

            if ( oconf->mode == OUTPUT_VARS ) {
            }

            /* Storing is done. Now move what's after processed
             * data to beginning of `buf`, update `index`.
             *
             * Detect if all read data (index+count) is longer
             * than what's before delimeter plus delimeter len */
            if ( index + count > ( found - buf ) + main_d_len ) {
                memmove( buf, found + main_d_len, index + count - ( ( found - buf ) + main_d_len ) );
                /* Index of first byte to write to = amount of moved data */
                index = index + count - ( ( found - buf ) + main_d_len );
                /* Be maximal sane, store null byte */
                buf[ index ] = '\0';
            } else {
                /* If we processed whole data package, so that we care
                 * just about newly readed bytes, then we set index to
                 * zero, as if we moved empty data to beginning and
                 * trailed with null byte */
                index = 0;
            }

        } else {
            /* Prepare index for read of next portion of data */
            index += count;
        }
    }

    /* Mark the thread as not working */
    worker_finished[ oconf->id ][ 0 ] = '1';

    /* Lower general workers counter */
    workers_count --;

    free_oconf_thread_safe( oconf );

    pthread_exit( NULL );
    return NULL;
}

/*
 * Options:
 * -a name - put input into global array `name'
 * -A name - put input into global hash `name', keys and values
 *           alternating
 * -x - put input into global variables, names and values determined
 *      as with hash (-d/-D); variables must already exist
 * -d string - main delimeter dividing into array elements
 * -D string - sub-delimeter, to divide into key and value
 */

static int
bin_zpopulator( char *name, char **argv, Options ops, int func )
{
    if ( OPT_ISSET( ops, 'h' ) ) {
        show_help();
        return 0;
    }

    if ( OPT_ISSET( ops, 'a' ) + OPT_ISSET( ops, 'A' ) + OPT_ISSET( ops, 'x' ) != 1 ) {
        if ( ! OPT_ISSET( ops, 's' ) ) {
            fprintf( stderr, "Error: Exactly one of following options is required: -a, -A, -x\n" );
            fprintf( stderr, "See help.\n" );
        } else {
            fprintf( stderr, "Require -a, -A or -x\n" );
        }
        fflush( stderr );
        return 1;
    }

    struct outconf *oconf = zalloc( sizeof( struct outconf ) );
    oconf->mode = OUTPUT_VARS;
    oconf->target = NULL;
    oconf->target_pm = NULL;
    oconf->main_d = ztrdup("\n");
    oconf->main_d_len = 1;
    oconf->sub_d = ztrdup(":");
    oconf->sub_d_len = 1;
    oconf->stream = NULL;
    oconf->err = NULL;
    oconf->r_devnull = NULL;

    int tries = 0;

duplicate_stderr:
    oconf->err = fdopen( dup( fileno( stderr ) ), "w" );

    ++ tries;

    if ( NULL == oconf->err || fileno( oconf->err ) == -1 ) {
        int file = oconf->err ? fileno( oconf->err ) : 0;
        fprintf( stderr, "Failed to duplicate stderr [%d]: %p (%d), %s\n", tries, oconf->err, file, strerror( errno ) );
        fflush( stderr );

        if ( tries < 8 ) {
            goto duplicate_stderr;
        } else {
            oconf->err = NULL;
            free_oconf( oconf );
            return 1;
        }
    }

    /* Submit the duplicated stderr FD to Zsh */
    addmodulefd( fileno( oconf->err ), FDT_MODULE );

    tries = 0;

duplicate_stdin:
    /* Duplicate standard input */
    oconf->stream = fdopen( dup( fileno( stdin ) ), "r" );
    /* Prepare standard input replacement */
    oconf->r_devnull = fopen( "/dev/null", "r");
    /* Replace standard input with /dev/null */
    dup2( fileno( oconf->r_devnull ), STDIN_FILENO );
    fclose( oconf->r_devnull );
    oconf->r_devnull = NULL;

    ++ tries;

    if ( NULL == oconf->stream || fileno( oconf->stream ) == -1 ) {
        int file = oconf->stream ? fileno( oconf->stream ) : 0;
        fprintf( stderr, "Failed to duplicate stream [%d]: %p (%d), %s\n", tries, oconf->stream, file, strerror( errno ) );
        fflush( stderr );
        if ( tries < 8 ) {
            goto duplicate_stdin;
        } else {
            oconf->stream = NULL;
            free_oconf( oconf );
            return 1;
        }
    }

    /* Submit the FD to Zsh */
    addmodulefd( fileno( oconf->stream ), FDT_MODULE );

    oconf->silent = OPT_ISSET( ops, 's' );
    oconf->only_global = OPT_ISSET( ops, 'g' );
    oconf->debug = OPT_ISSET( ops, 'v' );

    /* Array targets */
    if ( OPT_ISSET( ops, 'a' ) ) {
       oconf->mode = OUTPUT_ARRAY;
       oconf->target = ztrdup( OPT_ARG( ops, 'a' ) );
    } else if ( OPT_ISSET( ops, 'A' ) ) {
       oconf->mode = OUTPUT_HASH;
       oconf->target = ztrdup( OPT_ARG( ops, 'A' ) );
    }

    /* Delimeters */

    if ( OPT_ISSET( ops, 'd' ) ) {
        zsfree( oconf->main_d );
        oconf->main_d = ztrdup( OPT_ARG( ops, 'd' ) );
        oconf->main_d_len = strlen( oconf->main_d );
    }

    if ( OPT_ISSET( ops, 'D' ) ) {
        zsfree( oconf->sub_d );
        oconf->sub_d = ztrdup( OPT_ARG( ops, 'D' ) );
        oconf->sub_d_len = strlen( oconf->sub_d );
    }

    /* Worker ID */
    if ( *argv ) {
        oconf->id = atoi( *argv );
        oconf->id --;
        if ( oconf->id >= WORKER_COUNT || oconf->id < 0 ) {
            if ( ! oconf->silent ) {
                fprintf( stderr, "Worker thread ID should be from 1 to %d, aborting\n", WORKER_COUNT );
                fflush( stderr );
            }
            return 1;
        }
    } else {
        oconf->id = 0;
    }

    oconf->target_pm = ensurethereishash( oconf->target, oconf );
    if ( ! oconf->target_pm ) {
        free_oconf( oconf );
        return 1;
    }

    /* Mark the thread as working */
    worker_finished[ oconf->id ][ 0 ] = '0';

    /* Sum up the created worker thread */
    workers_count ++;

    /* Run the thread */
    if ( pthread_create( &workers[ oconf->id ], NULL, process_input, oconf ) ) {
        if ( ! oconf->silent ) {
            fprintf( stderr, "zpopulator: Error creating thread\n" );
            fflush( stderr );
        }

        free_oconf( oconf );

        return 1;
    }

    return 0;
}

/* this function is run by separate thread */

static void *eval_it( void *void_ptr ) {
    struct zpinconf *pconf = ( struct zpinconf * ) void_ptr;
    bin_eval( NULL, pconf->command, NULL, 0 );
    return NULL;
}

static int
bin_zpin( char *name, char **argv, Options ops, int func )
{
    if ( ! *argv ) {
        fprintf( stderr, "zpin expects string with command to execute\n" );
        fflush( stderr );
        return 1;
    }

    pid_t pid = getppid();

    if ( fork() ) {
        /* Prepare standard output replacement */
        FILE *w_devnull = fopen( "/dev/null", "w");
        /* Replace standard output with /dev/null */
        dup2( fileno( w_devnull ), STDOUT_FILENO );
        fclose( w_devnull );
        return 0;
    } else {
        kill( pid, SIGCHLD );
        struct zpinconf *pconf = zalloc( sizeof( struct zpinconf ) );
        pconf->command[0] = ztrdup( *argv );
        pconf->command[1] = NULL;

        eval_it( pconf );

        zsfree( pconf->command[0] );
        zfree( pconf, sizeof ( struct zpinconf ) );
    }

    return 0;
}

/*
 * boot_ is executed when the module is loaded.
 */

static struct builtin bintab[] = {
    BUILTIN("zpopulator", 0, bin_zpopulator, 0, -1, 0, "a:A:x:d:D:hsgv", NULL),
    BUILTIN("zpin", 0, bin_zpin, 0, -1, 0, "", NULL),
};

static struct paramdef patab[] = {
    ROINTPARAMDEF( "zpworkers_count", &workers_count ),
    ROARRPARAMDEF( "zpworker_finished", &worker_finished ),
};

static struct features module_features = {
    bintab, sizeof(bintab)/sizeof(*bintab),
    0, 0,
    0, 0,
    patab, sizeof(patab)/sizeof(*patab),
    0
};

/**/
int
setup_(UNUSED(Module m))
{
    return 0;
}

/**/
int
features_(Module m, char ***features)
{
    *features = featuresarray(m, &module_features);
    return 0;
}

/**/
int
enables_(Module m, int **enables)
{
    return handlefeatures(m, &module_features, enables);
}

/**/
int
boot_(Module m)
{
    worker_finished = zshcalloc( ( WORKER_COUNT + 1 ) * sizeof( char * ) );

    for ( int i = 0 ; i < WORKER_COUNT; i ++ ) {
        worker_finished[ i ] = ztrdup( "1" );
    }

    worker_finished[ WORKER_COUNT ] = NULL;

    return 0;
}

/**/
int
cleanup_(Module m)
{
    return setfeatureenables(m, &module_features, NULL);
}

/**/
int
finish_(UNUSED(Module m))
{
    printf( "zpopulator unloaded, bye.\n" );
    fflush( stdout );
    return 0;
}

/*********************************************************************/
/* Repeated hash functions with thread-safety amendments             */
/*********************************************************************/

static HashTable my_newparamtable(int size, char const *name)
{
    HashTable ht;
    if (!size)
	size = 17;
    ht = my_newhashtable(size, name, NULL);

    ht->hash        = hasher;
    ht->emptytable  = my_emptyhashtable;
    ht->filltable   = NULL;
    ht->cmpnodes    = strcmp;
    ht->addnode     = my_addhashnode;
    ht->getnode     = my_getparamnode;
    ht->getnode2    = my_gethashnode2;
    ht->removenode  = my_removehashnode;
    ht->disablenode = NULL;
    ht->enablenode  = NULL;
    ht->freenode    = my_freeparamnode;
    ht->printnode   = printparamnode;      /* safe, and used only after this module's computation */

    return ht;
}

static HashTable my_newhashtable(int size, UNUSED(char const *name), UNUSED(PrintTableStats printinfo)) {
    HashTable ht;

    ht = (HashTable) my_zshcalloc(sizeof *ht);
#ifdef ZSH_HASH_DEBUG
    ht->next = NULL;
    if(!firstht)
	firstht = ht;
    ht->last = lastht;
    if(lastht)
	lastht->next = ht;
    lastht = ht;
    ht->printinfo = printinfo ? printinfo : my_printhashtabinfo;
    ht->tablename = my_ztrdup(name);
#endif /* ZSH_HASH_DEBUG */
    ht->nodes = (HashNode *) my_zshcalloc(size * sizeof(HashNode));
    ht->hsize = size;
    ht->ct = 0;
    ht->scan = NULL;
    ht->scantab = NULL;
    return ht;
}

static void my_emptyhashtable(HashTable ht)
{
    my_resizehashtable(ht, ht->hsize);
}

static void my_resizehashtable(HashTable ht, int newsize)
{
    struct hashnode **ha, *hn, *hp;
    int i;

    /* free all the hash nodes */
    ha = ht->nodes;
    for (i = 0; i < ht->hsize; i++, ha++) {
	for (hn = *ha; hn;) {
	    hp = hn->next;
	    ht->freenode(hn);
	    hn = hp;
	}
    }

    /* If new size desired is different from current size, *
     * we free it and allocate a new nodes array.          */
    if (ht->hsize != newsize) {
	my_zfree(ht->nodes, ht->hsize * sizeof(HashNode));
	ht->nodes = (HashNode *) my_zshcalloc(newsize * sizeof(HashNode));
	ht->hsize = newsize;
    } else {
	/* else we just re-zero the current nodes array */
	memset(ht->nodes, 0, newsize * sizeof(HashNode));
    }

    ht->ct = 0;
}

static void my_addhashnode(HashTable ht, char *nam, void *nodeptr) {
    HashNode oldnode = my_addhashnode2(ht, nam, nodeptr);
    if (oldnode)
	ht->freenode(oldnode);
}

static HashNode my_addhashnode2(HashTable ht, char *nam, void *nodeptr) {
    unsigned hashval;
    HashNode hn, hp, hq;

    hn = (HashNode) nodeptr;
    hn->nam = nam;

    hashval = ht->hash(hn->nam) % ht->hsize;
    hp = ht->nodes[hashval];

    /* check if this is the first node for this hash value */
    if (!hp) {
	hn->next = NULL;
	ht->nodes[hashval] = hn;
	if (++ht->ct >= ht->hsize * 2 && !ht->scan)
	    my_expandhashtable(ht);
	return NULL;
    }

    /* else check if the first node contains the same key */
    if (ht->cmpnodes(hp->nam, hn->nam) == 0) {
	ht->nodes[hashval] = hn;
	replacing:
	hn->next = hp->next;
	if(ht->scan) {
	    if(ht->scan->sorted) {
		HashNode *hashtab = ht->scan->u.s.hashtab;
		int i;
		for(i = ht->scan->u.s.ct; i--; )
		    if(hashtab[i] == hp)
			hashtab[i] = hn;
	    } else if(ht->scan->u.u == hp)
		ht->scan->u.u = hn;
	}
	return hp;
    }

    /* else run through the list and check all the keys */
    hq = hp;
    hp = hp->next;
    for (; hp; hq = hp, hp = hp->next) {
	if (ht->cmpnodes(hp->nam, hn->nam) == 0) {
	    hq->next = hn;
	    goto replacing;
	}
    }

    /* else just add it at the front of the list */
    hn->next = ht->nodes[hashval];
    ht->nodes[hashval] = hn;
    if (++ht->ct >= ht->hsize * 2 && !ht->scan)
        my_expandhashtable(ht);
    return NULL;
}

static void my_expandhashtable(HashTable ht) {
    struct hashnode **onodes, **ha, *hn, *hp;
    int i, osize;

    osize = ht->hsize;
    onodes = ht->nodes;

    ht->hsize = osize * 4;
    ht->nodes = (HashNode *) my_zshcalloc(ht->hsize * sizeof(HashNode));
    ht->ct = 0;

    /* scan through the old list of nodes, and *
     * rehash them into the new list of nodes  */
    for (i = 0, ha = onodes; i < osize; i++, ha++) {
	for (hn = *ha; hn;) {
	    hp = hn->next;
	    ht->addnode(ht, hn->nam, hn);
	    hn = hp;
	}
    }
    my_zfree(onodes, osize * sizeof(HashNode));
}

static HashNode my_getparamnode(HashTable ht, const char *nam) {
    HashNode hn = my_gethashnode2(ht, nam); /* safe */

    /* Hashes created by this module will not have PM_AUTOLOAD flag */
#if 0
    if (pm && pm->u.str && (pm->node.flags & PM_AUTOLOAD)) {
    }
#endif

    return hn;
}

/* This function could be left without reimplementation -
 * it doesn't touch queue_signals or memory allocation */
static HashNode my_gethashnode2(HashTable ht, const char *nam) {
    unsigned hashval;
    HashNode hp;

    hashval = ht->hash(nam) % ht->hsize;
    for (hp = ht->nodes[hashval]; hp; hp = hp->next) {
	if (ht->cmpnodes(hp->nam, nam) == 0)
	    return hp;
    }
    return NULL;
}

/* This function could be left without reimplementation -
 * it doesn't touch queue_signals or memory allocation */
static HashNode my_removehashnode(HashTable ht, const char *nam) {
    unsigned hashval;
    HashNode hp, hq;

    hashval = ht->hash(nam) % ht->hsize;
    hp = ht->nodes[hashval];

    /* if no nodes at this hash value, return NULL */
    if (!hp)
	return NULL;

    /* else check if the key in the first one matches */
    if (ht->cmpnodes(hp->nam, nam) == 0) {
	ht->nodes[hashval] = hp->next;
	gotit:
	ht->ct--;
	if(ht->scan) {
	    if(ht->scan->sorted) {
		HashNode *hashtab = ht->scan->u.s.hashtab;
		int i;
		for(i = ht->scan->u.s.ct; i--; )
		    if(hashtab[i] == hp)
			hashtab[i] = NULL;
	    } else if(ht->scan->u.u == hp)
		ht->scan->u.u = hp->next;
	}
	return hp;
    }

    /* else run through the list and check the rest of the keys */
    hq = hp;
    hp = hp->next;
    for (; hp; hq = hp, hp = hp->next) {
	if (ht->cmpnodes(hp->nam, nam) == 0) {
	    hq->next = hp->next;
	    goto gotit;
	}
    }

    /* else it is not in the list, so return NULL */
    return NULL;
}

static void my_freeparamnode(HashNode hn) {
    Param pm = (Param) hn;

    /* The second argument of unsetfn() is used by modules to
     * differentiate "exp"licit unset from implicit unset, as when
     * a parameter is going out of scope.  It's not clear which
     * of these applies here, but passing 1 has always worked.
     *
     * zpopulator changes: delunset treated as always true. This
     * can lead to calling unsetfn twice, however this is safe,
     * because zsfree used in this function has NULL guard.
     */

    // if (delunset)
    pm->gsu.s->unsetfn(pm, 1);

    my_zsfree(pm->node.nam);
    /* If this variable was tied by the user, ename was ztrdup'd */
    if (pm->node.flags & PM_TIED)
	my_zsfree(pm->ename);
    my_zfree(pm, sizeof(struct param));
}

#ifdef ZSH_HASH_DEBUG
static void my_printhashtabinfo(HashTable ht) {
    HashNode hn;
    int chainlen[MAXDEPTH + 1];
    int i, tmpcount, total;

    printf("name of table   : %s\n",   ht->tablename);
    printf("size of nodes[] : %d\n",   ht->hsize);
    printf("number of nodes : %d\n\n", ht->ct);
    fflush( stdout );

    memset(chainlen, 0, sizeof(chainlen));

    /* count the number of nodes just to be sure */
    total = 0;
    for (i = 0; i < ht->hsize; i++) {
	tmpcount = 0;
	for (hn = ht->nodes[i]; hn; hn = hn->next)
	    tmpcount++;
	if (tmpcount >= MAXDEPTH)
	    chainlen[MAXDEPTH]++;
	else
	    chainlen[tmpcount]++;
	total += tmpcount;
    }

    for (i = 0; i < MAXDEPTH; i++)
	printf("number of hash values with chain of length %d  : %4d\n", i, chainlen[i]);
    printf("number of hash values with chain of length %d+ : %4d\n", MAXDEPTH, chainlen[MAXDEPTH]);
    printf("total number of nodes                         : %4d\n", total);
    fflush( stdout );
}
#endif

/*********************************************************************/
/* Memory management functions                                       */
/*********************************************************************/

static void * my_zshcalloc(size_t size)
{
    void *ptr = my_zalloc(size);
    if (!size) {
	size = 1;
    }
    memset(ptr, 0, size);
    return ptr;
}

static void * my_zalloc(size_t size)
{
    void *ptr;

    if (!size)
	size = 1;

    if (!(ptr = (void *) malloc(size))) {
	fputs( "zpopulator: fatal error: out of memory", stderr );
        fflush( stderr );
    }

    return ptr;
}

static void my_zfree(void *p, UNUSED(int sz)) {
    if (p)
	free(p);
}

static void my_zsfree(char *p) {
    if (p)
	free(p);
}

static void * my_zrealloc(void *ptr, size_t size) {
    if (ptr) {
	if (size) {
	    /* Do normal realloc */
	    if (!(ptr = (void *) realloc(ptr, size))) {
		zerr("zpopulator: fatal error: out of memory");
		exit(1);
	    }
	    return ptr;
	}
	else {
	    /* If ptr is not NULL, but size is zero, *
	     * then object pointed to is freed.      */
	    free(ptr);
        }

	ptr = NULL;
    } else {
	/* If ptr is NULL, then behave like malloc */
        if (!(ptr = (void *) malloc(size))) {
            zerr("zpopulator: fatal error: out of memory");
            exit(1);
        }
    }

    return ptr;
}

static char * my_ztrdup(const char *s) {
    char *t;

    if (!s)
	return NULL;
    t = (char *)my_zalloc(strlen((char *)s) + 1);
    strcpy(t, s);
    return t;
}

/*********************************************************************/
/* Thread safe setters and getters, although they can be used only   */
/* within computation thread, because they don't queue signals, etc. */
/* Left this converted code, though assignments are done directly.   */
/*********************************************************************/

static const struct gsu_scalar my_stdscalar_gsu = { my_strgetfn, my_strsetfn, my_stdunsetfn };

static void my_assigngetset(Param pm) {
    switch (PM_TYPE(pm->node.flags)) {
    case PM_SCALAR:
	pm->gsu.s = &my_stdscalar_gsu;
	break;
    case PM_ARRAY:
	pm->gsu.a = &stdarray_gsu;
	break;
    case PM_HASHED:
	pm->gsu.h = &stdhash_gsu;
	break;
    default:
	DPUTS(1, "BUG: tried to create param node without valid flag");
	break;
    }
}

static char * my_strgetfn(Param pm) {
    return pm->u.str ? pm->u.str : (char *) "";
}

static void my_strsetfn(Param pm, char *x) {
    my_zsfree(pm->u.str);
    pm->u.str = x;

    /* If you update this function, you may need to update the
     * `Implement remainder of strsetfn' block in assignstrvalue(). */
}

static void my_stdunsetfn(Param pm, UNUSED(int exp)) {
    switch (PM_TYPE(pm->node.flags)) {
	case PM_SCALAR:
	    if (pm->gsu.s->setfn)
		pm->gsu.s->setfn(pm, NULL);
	    break;

	case PM_ARRAY:
	    if (pm->gsu.a->setfn)
		pm->gsu.a->setfn(pm, NULL);
	    break;

	case PM_HASHED:
	    if (pm->gsu.h->setfn)
		pm->gsu.h->setfn(pm, NULL);
	    break;

	default:
	    if (!(pm->node.flags & PM_SPECIAL))
                pm->u.str = NULL;
	    break;
    }
    if ((pm->node.flags & (PM_SPECIAL|PM_TIED)) == PM_TIED) {
	if (pm->ename) {
	    my_zsfree(pm->ename);
	    pm->ename = NULL;
	}
	pm->node.flags &= ~PM_TIED;
    }
    pm->node.flags |= PM_UNSET;
}

