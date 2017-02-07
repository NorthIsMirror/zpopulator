/*
 * zpopulator – multi-threading support for Zsh
 *
 * Copyright (c) 2017 Sebastian Gniazdowski
 * All rights reserved.
 *
 * Licensed under MIT, GPLv3.
 */

#include "zpopulator.mdh"
#include "zpopulator.pro"

/**/
static int
bin_zpopulator(char *name, char **argv, Options ops, int func)
{
    printf("zpopulator called\n");
    fflush(stdout);
    return 0;
}

/*
 * boot_ is executed when the module is loaded.
 */

static struct builtin bintab[] = {
    BUILTIN("zpopulator", 0, bin_zpopulator, 0, -1, 0, "", NULL),
};

static struct features module_features = {
    bintab, sizeof(bintab)/sizeof(*bintab),
    0
};

/**/
int
setup_(UNUSED(Module m))
{
    printf("The example module has now been set up.\n");
    fflush(stdout);
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
    printf("Thank you for using the example module. Have a nice day.\n");
    fflush(stdout);
    return 0;
}
