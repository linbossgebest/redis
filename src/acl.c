/*
 * Copyright (c) 2018, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "sha256.h"
#include <fcntl.h>
#include <ctype.h>

/* =============================================================================
 * Global state for ACLs
 * ==========================================================================*/

rax *Users; /* Table mapping usernames to user structures. */

user *DefaultUser;  /* Global reference to the default user.
                       Every new connection is associated to it, if no
                       AUTH or HELLO is used to authenticate with a
                       different user. */

list *UsersToLoad;  /* This is a list of users found in the configuration file
                       that we'll need to load in the final stage of Redis
                       initialization, after all the modules are already
                       loaded. Every list element is a NULL terminated
                       array of SDS pointers: the first is the user name,
                       all the remaining pointers are ACL rules in the same
                       format as ACLSetUser(). */
list *ACLLog;       /* Our security log, the user is able to inspect that
                       using the ACL LOG command .*/

static rax *commandId = NULL; /* Command name to id mapping */

static unsigned long nextid = 0; /* Next command id that has not been assigned */

struct ACLCategoryItem {
    const char *name;
    uint64_t flag;
} ACLCommandCategories[] = { /* See redis.conf for details on each category. */
    {"keyspace", CMD_CATEGORY_KEYSPACE},
    {"read", CMD_CATEGORY_READ},
    {"write", CMD_CATEGORY_WRITE},
    {"set", CMD_CATEGORY_SET},
    {"sortedset", CMD_CATEGORY_SORTEDSET},
    {"list", CMD_CATEGORY_LIST},
    {"hash", CMD_CATEGORY_HASH},
    {"string", CMD_CATEGORY_STRING},
    {"bitmap", CMD_CATEGORY_BITMAP},
    {"hyperloglog", CMD_CATEGORY_HYPERLOGLOG},
    {"geo", CMD_CATEGORY_GEO},
    {"stream", CMD_CATEGORY_STREAM},
    {"pubsub", CMD_CATEGORY_PUBSUB},
    {"admin", CMD_CATEGORY_ADMIN},
    {"fast", CMD_CATEGORY_FAST},
    {"slow", CMD_CATEGORY_SLOW},
    {"blocking", CMD_CATEGORY_BLOCKING},
    {"dangerous", CMD_CATEGORY_DANGEROUS},
    {"connection", CMD_CATEGORY_CONNECTION},
    {"transaction", CMD_CATEGORY_TRANSACTION},
    {"scripting", CMD_CATEGORY_SCRIPTING},
    {NULL,0} /* Terminator. */
};

struct ACLUserFlag {
    const char *name;
    uint64_t flag;
} ACLUserFlags[] = {
    /* Note: the order here dictates the emitted order at ACLDescribeUser */
    {"on", USER_FLAG_ENABLED},
    {"off", USER_FLAG_DISABLED},
    {"allkeys", USER_FLAG_ALLKEYS},
    {"allchannels", USER_FLAG_ALLCHANNELS},
    {"allcommands", USER_FLAG_ALLCOMMANDS},
    {"nopass", USER_FLAG_NOPASS},
    {"skip-sanitize-payload", USER_FLAG_SANITIZE_PAYLOAD_SKIP},
    {"sanitize-payload", USER_FLAG_SANITIZE_PAYLOAD},
    {NULL,0} /* Terminator. */
};

void ACLResetFirstArgsForCommand(user *u, unsigned long id);
void ACLResetFirstArgs(user *u);
void ACLAddAllowedFirstArg(user *u, unsigned long id, const char *sub);
void ACLFreeLogEntry(void *le);

/* The length of the string representation of a hashed password. */
#define HASH_PASSWORD_LEN SHA256_BLOCK_SIZE*2

/* =============================================================================
 * Helper functions for the rest of the ACL implementation
 * ==========================================================================*/

/* Return zero if strings are the same, non-zero if they are not.
 * The comparison is performed in a way that prevents an attacker to obtain
 * information about the nature of the strings just monitoring the execution
 * time of the function.
 *
 * Note that limiting the comparison length to strings up to 512 bytes we
 * can avoid leaking any information about the password length and any
 * possible branch misprediction related leak.
 */
int time_independent_strcmp(char *a, char *b) {
    char bufa[CONFIG_AUTHPASS_MAX_LEN], bufb[CONFIG_AUTHPASS_MAX_LEN];
    /* The above two strlen perform len(a) + len(b) operations where either
     * a or b are fixed (our password) length, and the difference is only
     * relative to the length of the user provided string, so no information
     * leak is possible in the following two lines of code. */
    unsigned int alen = strlen(a);
    unsigned int blen = strlen(b);
    unsigned int j;
    int diff = 0;

    /* We can't compare strings longer than our static buffers.
     * Note that this will never pass the first test in practical circumstances
     * so there is no info leak. */
    if (alen > sizeof(bufa) || blen > sizeof(bufb)) return 1;

    memset(bufa,0,sizeof(bufa));        /* Constant time. */
    memset(bufb,0,sizeof(bufb));        /* Constant time. */
    /* Again the time of the following two copies is proportional to
     * len(a) + len(b) so no info is leaked. */
    memcpy(bufa,a,alen);
    memcpy(bufb,b,blen);

    /* Always compare all the chars in the two buffers without
     * conditional expressions. */
    for (j = 0; j < sizeof(bufa); j++) {
        diff |= (bufa[j] ^ bufb[j]);
    }
    /* Length must be equal as well. */
    diff |= alen ^ blen;
    return diff; /* If zero strings are the same. */
}

/* Given an SDS string, returns the SHA256 hex representation as a
 * new SDS string. */
sds ACLHashPassword(unsigned char *cleartext, size_t len) {
    SHA256_CTX ctx;
    unsigned char hash[SHA256_BLOCK_SIZE];
    char hex[HASH_PASSWORD_LEN];
    char *cset = "0123456789abcdef";

    sha256_init(&ctx);
    sha256_update(&ctx,(unsigned char*)cleartext,len);
    sha256_final(&ctx,hash);

    for (int j = 0; j < SHA256_BLOCK_SIZE; j++) {
        hex[j*2] = cset[((hash[j]&0xF0)>>4)];
        hex[j*2+1] = cset[(hash[j]&0xF)];
    }
    return sdsnewlen(hex,HASH_PASSWORD_LEN);
}

/* Given a hash and the hash length, returns C_OK if it is a valid password
 * hash, or C_ERR otherwise. */
int ACLCheckPasswordHash(unsigned char *hash, int hashlen) {
    if (hashlen != HASH_PASSWORD_LEN) {
        return C_ERR;
    }

    /* Password hashes can only be characters that represent
     * hexadecimal values, which are numbers and lowercase
     * characters 'a' through 'f'. */
    for(int i = 0; i < HASH_PASSWORD_LEN; i++) {
        char c = hash[i];
        if ((c < 'a' || c > 'f') && (c < '0' || c > '9')) {
            return C_ERR;
        }
    }
    return C_OK;
}

/* =============================================================================
 * Low level ACL API
 * ==========================================================================*/

/* Return 1 if the specified string contains spaces or null characters.
 * We do this for usernames and key patterns for simpler rewriting of
 * ACL rules, presentation on ACL list, and to avoid subtle security bugs
 * that may arise from parsing the rules in presence of escapes.
 * The function returns 0 if the string has no spaces. */
int ACLStringHasSpaces(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (isspace(s[i]) || s[i] == 0) return 1;
    }
    return 0;
}

/* Given the category name the command returns the corresponding flag, or
 * zero if there is no match. */
uint64_t ACLGetCommandCategoryFlagByName(const char *name) {
    for (int j = 0; ACLCommandCategories[j].flag != 0; j++) {
        if (!strcasecmp(name,ACLCommandCategories[j].name)) {
            return ACLCommandCategories[j].flag;
        }
    }
    return 0; /* No match. */
}

/* Method for searching for a user within a list of user definitions. The
 * list contains an array of user arguments, and we are only
 * searching the first argument, the username, for a match. */
int ACLListMatchLoadedUser(void *definition, void *user) {
    sds *user_definition = definition;
    return sdscmp(user_definition[0], user) == 0;
}

/* Method for passwords/pattern comparison used for the user->passwords list
 * so that we can search for items with listSearchKey(). */
int ACLListMatchSds(void *a, void *b) {
    return sdscmp(a,b) == 0;
}

/* Method to free list elements from ACL users password/patterns lists. */
void ACLListFreeSds(void *item) {
    sdsfree(item);
}

/* Method to duplicate list elements from ACL users password/patterns lists. */
void *ACLListDupSds(void *item) {
    return sdsdup(item);
}

/* Create a new user with the specified name, store it in the list
 * of users (the Users global radix tree), and returns a reference to
 * the structure representing the user.
 *
 * If the user with such name already exists NULL is returned. */
user *ACLCreateUser(const char *name, size_t namelen) {
    if (raxFind(Users,(unsigned char*)name,namelen) != raxNotFound) return NULL;
    user *u = zmalloc(sizeof(*u));
    u->name = sdsnewlen(name,namelen);
    u->flags = USER_FLAG_DISABLED | server.acl_pubsub_default;
    u->allowed_firstargs = NULL;
    u->passwords = listCreate();
    u->patterns = listCreate();
    u->channels = listCreate();
    listSetMatchMethod(u->passwords,ACLListMatchSds);
    listSetFreeMethod(u->passwords,ACLListFreeSds);
    listSetDupMethod(u->passwords,ACLListDupSds);
    listSetMatchMethod(u->patterns,ACLListMatchSds);
    listSetFreeMethod(u->patterns,ACLListFreeSds);
    listSetDupMethod(u->patterns,ACLListDupSds);
    listSetMatchMethod(u->channels,ACLListMatchSds);
    listSetFreeMethod(u->channels,ACLListFreeSds);
    listSetDupMethod(u->channels,ACLListDupSds);
    memset(u->allowed_commands,0,sizeof(u->allowed_commands));
    raxInsert(Users,(unsigned char*)name,namelen,u,NULL);
    return u;
}

/* This function should be called when we need an unlinked "fake" user
 * we can use in order to validate ACL rules or for other similar reasons.
 * The user will not get linked to the Users radix tree. The returned
 * user should be released with ACLFreeUser() as usually. */
user *ACLCreateUnlinkedUser(void) {
    char username[64];
    for (int j = 0; ; j++) {
        snprintf(username,sizeof(username),"__fakeuser:%d__",j);
        user *fakeuser = ACLCreateUser(username,strlen(username));
        if (fakeuser == NULL) continue;
        int retval = raxRemove(Users,(unsigned char*) username,
                               strlen(username),NULL);
        serverAssert(retval != 0);
        return fakeuser;
    }
}

/* Release the memory used by the user structure. Note that this function
 * will not remove the user from the Users global radix tree. */
void ACLFreeUser(user *u) {
    sdsfree(u->name);
    listRelease(u->passwords);
    listRelease(u->patterns);
    listRelease(u->channels);
    ACLResetFirstArgs(u);
    zfree(u);
}

/* When a user is deleted we need to cycle the active
 * connections in order to kill all the pending ones that
 * are authenticated with such user. */
void ACLFreeUserAndKillClients(user *u) {
    listIter li;
    listNode *ln;
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (c->user == u) {
            /* We'll free the connection asynchronously, so
             * in theory to set a different user is not needed.
             * However if there are bugs in Redis, soon or later
             * this may result in some security hole: it's much
             * more defensive to set the default user and put
             * it in non authenticated mode. */
            c->user = DefaultUser;
            c->authenticated = 0;
            /* We will write replies to this client later, so we can't
             * close it directly even if async. */
            if (c == server.current_client) {
                c->flags |= CLIENT_CLOSE_AFTER_COMMAND;
            } else {
                freeClientAsync(c);
            }
        }
    }
    ACLFreeUser(u);
}

/* Copy the user ACL rules from the source user 'src' to the destination
 * user 'dst' so that at the end of the process they'll have exactly the
 * same rules (but the names will continue to be the original ones). */
void ACLCopyUser(user *dst, user *src) {
    listRelease(dst->passwords);
    listRelease(dst->patterns);
    listRelease(dst->channels);
    dst->passwords = listDup(src->passwords);
    dst->patterns = listDup(src->patterns);
    dst->channels = listDup(src->channels);
    memcpy(dst->allowed_commands,src->allowed_commands,
           sizeof(dst->allowed_commands));
    dst->flags = src->flags;
    ACLResetFirstArgs(dst);
    /* Copy the allowed first-args array of array of SDS strings. */
    if (src->allowed_firstargs) {
        for (int j = 0; j < USER_COMMAND_BITS_COUNT; j++) {
            if (src->allowed_firstargs[j]) {
                for (int i = 0; src->allowed_firstargs[j][i]; i++)
                {
                    ACLAddAllowedFirstArg(dst, j,
                        src->allowed_firstargs[j][i]);
                }
            }
        }
    }
}

/* Free all the users registered in the radix tree 'users' and free the
 * radix tree itself. */
void ACLFreeUsersSet(rax *users) {
    raxFreeWithCallback(users,(void(*)(void*))ACLFreeUserAndKillClients);
}

/* Given a command ID, this function set by reference 'word' and 'bit'
 * so that user->allowed_commands[word] will address the right word
 * where the corresponding bit for the provided ID is stored, and
 * so that user->allowed_commands[word]&bit will identify that specific
 * bit. The function returns C_ERR in case the specified ID overflows
 * the bitmap in the user representation. */
int ACLGetCommandBitCoordinates(uint64_t id, uint64_t *word, uint64_t *bit) {
    if (id >= USER_COMMAND_BITS_COUNT) return C_ERR;
    *word = id / sizeof(uint64_t) / 8;
    *bit = 1ULL << (id % (sizeof(uint64_t) * 8));
    return C_OK;
}

/* Check if the specified command bit is set for the specified user.
 * The function returns 1 is the bit is set or 0 if it is not.
 * Note that this function does not check the ALLCOMMANDS flag of the user
 * but just the lowlevel bitmask.
 *
 * If the bit overflows the user internal representation, zero is returned
 * in order to disallow the execution of the command in such edge case. */
int ACLGetUserCommandBit(const user *u, unsigned long id) {
    uint64_t word, bit;
    if (ACLGetCommandBitCoordinates(id,&word,&bit) == C_ERR) return 0;
    return (u->allowed_commands[word] & bit) != 0;
}

/* When +@all or allcommands is given, we set a reserved bit as well that we
 * can later test, to see if the user has the right to execute "future commands",
 * that is, commands loaded later via modules. */
int ACLUserCanExecuteFutureCommands(user *u) {
    return ACLGetUserCommandBit(u,USER_COMMAND_BITS_COUNT-1);
}

/* Set the specified command bit for the specified user to 'value' (0 or 1).
 * If the bit overflows the user internal representation, no operation
 * is performed. As a side effect of calling this function with a value of
 * zero, the user flag ALLCOMMANDS is cleared since it is no longer possible
 * to skip the command bit explicit test. */
void ACLSetUserCommandBit(user *u, unsigned long id, int value) {
    uint64_t word, bit;
    if (ACLGetCommandBitCoordinates(id,&word,&bit) == C_ERR) return;
    if (value) {
        u->allowed_commands[word] |= bit;
    } else {
        u->allowed_commands[word] &= ~bit;
        u->flags &= ~USER_FLAG_ALLCOMMANDS;
    }
}

/* This function is used to allow/block a specific command.
 * Allowing/blocking a container command also applies for its subcommands */
void ACLChangeCommandPerm(user *u, struct redisCommand *cmd, int allow) {
    unsigned long id = cmd->id;
    ACLSetUserCommandBit(u,id,allow);
    ACLResetFirstArgsForCommand(u,id);
    if (cmd->subcommands_dict) {
        dictEntry *de;
        dictIterator *di = dictGetSafeIterator(cmd->subcommands_dict);
        while((de = dictNext(di)) != NULL) {
            struct redisCommand *sub = (struct redisCommand *)dictGetVal(de);
            ACLSetUserCommandBit(u,sub->id,allow);
        }
        dictReleaseIterator(di);
    }
}

void ACLSetUserCommandBitsForCategoryLogic(dict *commands, user *u, uint64_t cflag, int value) {
    dictIterator *di = dictGetIterator(commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (cmd->flags & CMD_MODULE) continue; /* Ignore modules commands. */
        if (cmd->flags & cflag) {
            ACLChangeCommandPerm(u,cmd,value);
        }
        if (cmd->subcommands_dict) {
            ACLSetUserCommandBitsForCategoryLogic(cmd->subcommands_dict, u, cflag, value);
        }
    }
    dictReleaseIterator(di);
}

/* This is like ACLSetUserCommandBit(), but instead of setting the specified
 * ID, it will check all the commands in the category specified as argument,
 * and will set all the bits corresponding to such commands to the specified
 * value. Since the category passed by the user may be non existing, the
 * function returns C_ERR if the category was not found, or C_OK if it was
 * found and the operation was performed. */
int ACLSetUserCommandBitsForCategory(user *u, const char *category, int value) {
    uint64_t cflag = ACLGetCommandCategoryFlagByName(category);
    if (!cflag) return C_ERR;
    ACLSetUserCommandBitsForCategoryLogic(server.orig_commands, u, cflag, value);
    return C_OK;
}

void ACLCountCategoryBitsForCommands(dict *commands, user *u, unsigned long *on, unsigned long *off, uint64_t cflag) {
    dictIterator *di = dictGetIterator(commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (cmd->flags & cflag) {
            if (ACLGetUserCommandBit(u,cmd->id))
                (*on)++;
            else
                (*off)++;
        }
        if (cmd->subcommands_dict) {
            ACLCountCategoryBitsForCommands(cmd->subcommands_dict, u, on, off, cflag);
        }
    }
    dictReleaseIterator(di);
}

/* Return the number of commands allowed (on) and denied (off) for the user 'u'
 * in the subset of commands flagged with the specified category name.
 * If the category name is not valid, C_ERR is returned, otherwise C_OK is
 * returned and on and off are populated by reference. */
int ACLCountCategoryBitsForUser(user *u, unsigned long *on, unsigned long *off,
                                const char *category)
{
    uint64_t cflag = ACLGetCommandCategoryFlagByName(category);
    if (!cflag) return C_ERR;

    *on = *off = 0;
    ACLCountCategoryBitsForCommands(server.orig_commands, u, on, off, cflag);
    return C_OK;
}

sds ACLDescribeUserCommandRulesSingleCommands(user *u, user *fakeuser, sds rules, dict *commands) {
    dictIterator *di = dictGetIterator(commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        int userbit = ACLGetUserCommandBit(u,cmd->id);
        int fakebit = ACLGetUserCommandBit(fakeuser,cmd->id);
        if (userbit != fakebit) {
            rules = sdscatlen(rules, userbit ? "+" : "-", 1);
            sds fullname = getFullCommandName(cmd);
            rules = sdscat(rules,fullname);
            sdsfree(fullname);
            rules = sdscatlen(rules," ",1);
            ACLChangeCommandPerm(fakeuser,cmd,userbit);
        }

        if (cmd->subcommands_dict)
            rules = ACLDescribeUserCommandRulesSingleCommands(u,fakeuser,rules,cmd->subcommands_dict);

        /* Emit the first-args if there are any. */
        if (userbit == 0 && u->allowed_firstargs &&
            u->allowed_firstargs[cmd->id])
        {
            for (int j = 0; u->allowed_firstargs[cmd->id][j]; j++) {
                rules = sdscatlen(rules,"+",1);
                sds fullname = getFullCommandName(cmd);
                rules = sdscat(rules,fullname);
                sdsfree(fullname);
                rules = sdscatlen(rules,"|",1);
                rules = sdscatsds(rules,u->allowed_firstargs[cmd->id][j]);
                rules = sdscatlen(rules," ",1);
            }
        }
    }
    dictReleaseIterator(di);
    return rules;
}

/* This function returns an SDS string representing the specified user ACL
 * rules related to command execution, in the same format you could set them
 * back using ACL SETUSER. The function will return just the set of rules needed
 * to recreate the user commands bitmap, without including other user flags such
 * as on/off, passwords and so forth. The returned string always starts with
 * the +@all or -@all rule, depending on the user bitmap, and is followed, if
 * needed, by the other rules needed to narrow or extend what the user can do. */
sds ACLDescribeUserCommandRules(user *u) {
    sds rules = sdsempty();
    int additive;   /* If true we start from -@all and add, otherwise if
                       false we start from +@all and remove. */

    /* This code is based on a trick: as we generate the rules, we apply
     * them to a fake user, so that as we go we still know what are the
     * bit differences we should try to address by emitting more rules. */
    user fu = {0};
    user *fakeuser = &fu;

    /* Here we want to understand if we should start with +@all and remove
     * the commands corresponding to the bits that are not set in the user
     * commands bitmap, or the contrary. Note that semantically the two are
     * different. For instance starting with +@all and subtracting, the user
     * will be able to execute future commands, while -@all and adding will just
     * allow the user the run the selected commands and/or categories.
     * How do we test for that? We use the trick of a reserved command ID bit
     * that is set only by +@all (and its alias "allcommands"). */
    if (ACLUserCanExecuteFutureCommands(u)) {
        additive = 0;
        rules = sdscat(rules,"+@all ");
        ACLSetUser(fakeuser,"+@all",-1);
    } else {
        additive = 1;
        rules = sdscat(rules,"-@all ");
        ACLSetUser(fakeuser,"-@all",-1);
    }

    /* Attempt to find a good approximation for categories and commands
     * based on the current bits used, by looping over the category list
     * and applying the best fit each time. Often a set of categories will not 
     * perfectly match the set of commands into it, so at the end we do a 
     * final pass adding/removing the single commands needed to make the bitmap
     * exactly match. A temp user is maintained to keep track of categories 
     * already applied. */
    user tu = {0};
    user *tempuser = &tu;
    
    /* Keep track of the categories that have been applied, to prevent
     * applying them twice.  */
    char applied[sizeof(ACLCommandCategories)/sizeof(ACLCommandCategories[0])];
    memset(applied, 0, sizeof(applied));

    memcpy(tempuser->allowed_commands,
        u->allowed_commands, 
        sizeof(u->allowed_commands));
    while (1) {
        int best = -1;
        unsigned long mindiff = INT_MAX, maxsame = 0;
        for (int j = 0; ACLCommandCategories[j].flag != 0; j++) {
            if (applied[j]) continue;

            unsigned long on, off, diff, same;
            ACLCountCategoryBitsForUser(tempuser,&on,&off,ACLCommandCategories[j].name);
            /* Check if the current category is the best this loop:
             * * It has more commands in common with the user than commands
             *   that are different.
             * AND EITHER
             * * It has the fewest number of differences
             *    than the best match we have found so far. 
             * * OR it matches the fewest number of differences
             *   that we've seen but it has more in common. */
            diff = additive ? off : on;
            same = additive ? on : off;
            if (same > diff && 
                ((diff < mindiff) || (diff == mindiff && same > maxsame)))
            {
                best = j;
                mindiff = diff;
                maxsame = same;
            }
        }

        /* We didn't find a match */
        if (best == -1) break;

        sds op = sdsnewlen(additive ? "+@" : "-@", 2);
        op = sdscat(op,ACLCommandCategories[best].name);
        ACLSetUser(fakeuser,op,-1);

        sds invop = sdsnewlen(additive ? "-@" : "+@", 2);
        invop = sdscat(invop,ACLCommandCategories[best].name);
        ACLSetUser(tempuser,invop,-1);

        rules = sdscatsds(rules,op);
        rules = sdscatlen(rules," ",1);
        sdsfree(op);
        sdsfree(invop);

        applied[best] = 1;
    }

    /* Fix the final ACLs with single commands differences. */
    rules = ACLDescribeUserCommandRulesSingleCommands(u,fakeuser,rules,server.orig_commands);

    /* Trim the final useless space. */
    sdsrange(rules,0,-2);

    /* This is technically not needed, but we want to verify that now the
     * predicted bitmap is exactly the same as the user bitmap, and abort
     * otherwise, because aborting is better than a security risk in this
     * code path. */
    if (memcmp(fakeuser->allowed_commands,
                        u->allowed_commands,
                        sizeof(u->allowed_commands)) != 0)
    {
        serverLog(LL_WARNING,
            "CRITICAL ERROR: User ACLs don't match final bitmap: '%s'",
            rules);
        serverPanic("No bitmap match in ACLDescribeUserCommandRules()");
    }
    return rules;
}

/* This is similar to ACLDescribeUserCommandRules(), however instead of
 * describing just the user command rules, everything is described: user
 * flags, keys, passwords and finally the command rules obtained via
 * the ACLDescribeUserCommandRules() function. This is the function we call
 * when we want to rewrite the configuration files describing ACLs and
 * in order to show users with ACL LIST. */
sds ACLDescribeUser(user *u) {
    sds res = sdsempty();

    /* Flags. */
    for (int j = 0; ACLUserFlags[j].flag; j++) {
        /* Skip the allcommands, allkeys and allchannels flags because they'll
         * be emitted later as +@all, ~* and &*. */
        if (ACLUserFlags[j].flag == USER_FLAG_ALLKEYS ||
            ACLUserFlags[j].flag == USER_FLAG_ALLCHANNELS ||
            ACLUserFlags[j].flag == USER_FLAG_ALLCOMMANDS) continue;
        if (u->flags & ACLUserFlags[j].flag) {
            res = sdscat(res,ACLUserFlags[j].name);
            res = sdscatlen(res," ",1);
        }
    }

    /* Passwords. */
    listIter li;
    listNode *ln;
    listRewind(u->passwords,&li);
    while((ln = listNext(&li))) {
        sds thispass = listNodeValue(ln);
        res = sdscatlen(res,"#",1);
        res = sdscatsds(res,thispass);
        res = sdscatlen(res," ",1);
    }

    /* Key patterns. */
    if (u->flags & USER_FLAG_ALLKEYS) {
        res = sdscatlen(res,"~* ",3);
    } else {
        listRewind(u->patterns,&li);
        while((ln = listNext(&li))) {
            sds thispat = listNodeValue(ln);
            res = sdscatlen(res,"~",1);
            res = sdscatsds(res,thispat);
            res = sdscatlen(res," ",1);
        }
    }

    /* Pub/sub channel patterns. */
    if (u->flags & USER_FLAG_ALLCHANNELS) {
        res = sdscatlen(res,"&* ",3);
    } else {
        res = sdscatlen(res,"resetchannels ",14);
        listRewind(u->channels,&li);
        while((ln = listNext(&li))) {
            sds thispat = listNodeValue(ln);
            res = sdscatlen(res,"&",1);
            res = sdscatsds(res,thispat);
            res = sdscatlen(res," ",1);
        }
    }

    /* Command rules. */
    sds rules = ACLDescribeUserCommandRules(u);
    res = sdscatsds(res,rules);
    sdsfree(rules);
    return res;
}

/* Get a command from the original command table, that is not affected
 * by the command renaming operations: we base all the ACL work from that
 * table, so that ACLs are valid regardless of command renaming. */
struct redisCommand *ACLLookupCommand(const char *name) {
    struct redisCommand *cmd;
    sds sdsname = sdsnew(name);
    cmd = lookupCommandBySdsLogic(server.orig_commands,sdsname);
    sdsfree(sdsname);
    return cmd;
}

/* Flush the array of allowed first-args for the specified user
 * and command ID. */
void ACLResetFirstArgsForCommand(user *u, unsigned long id) {
    if (u->allowed_firstargs && u->allowed_firstargs[id]) {
        for (int i = 0; u->allowed_firstargs[id][i]; i++)
            sdsfree(u->allowed_firstargs[id][i]);
        zfree(u->allowed_firstargs[id]);
        u->allowed_firstargs[id] = NULL;
    }
}

/* Flush the entire table of first-args. This is useful on +@all, -@all
 * or similar to return back to the minimal memory usage (and checks to do)
 * for the user. */
void ACLResetFirstArgs(user *u) {
    if (u->allowed_firstargs == NULL) return;
    for (int j = 0; j < USER_COMMAND_BITS_COUNT; j++) {
        if (u->allowed_firstargs[j]) {
            for (int i = 0; u->allowed_firstargs[j][i]; i++)
                sdsfree(u->allowed_firstargs[j][i]);
            zfree(u->allowed_firstargs[j]);
        }
    }
    zfree(u->allowed_firstargs);
    u->allowed_firstargs = NULL;
}

/* Add a first-arh to the list of subcommands for the user 'u' and
 * the command id specified. */
void ACLAddAllowedFirstArg(user *u, unsigned long id, const char *sub) {
    /* If this is the first first-arg to be configured for
     * this user, we have to allocate the first-args array. */
    if (u->allowed_firstargs == NULL) {
        u->allowed_firstargs = zcalloc(USER_COMMAND_BITS_COUNT * sizeof(sds*));
    }

    /* We also need to enlarge the allocation pointing to the
     * null terminated SDS array, to make space for this one.
     * To start check the current size, and while we are here
     * make sure the first-arg is not already specified inside. */
    long items = 0;
    if (u->allowed_firstargs[id]) {
        while(u->allowed_firstargs[id][items]) {
            /* If it's already here do not add it again. */
            if (!strcasecmp(u->allowed_firstargs[id][items],sub))
                return;
            items++;
        }
    }

    /* Now we can make space for the new item (and the null term). */
    items += 2;
    u->allowed_firstargs[id] = zrealloc(u->allowed_firstargs[id], sizeof(sds)*items);
    u->allowed_firstargs[id][items-2] = sdsnew(sub);
    u->allowed_firstargs[id][items-1] = NULL;
}

/* Set user properties according to the string "op". The following
 * is a description of what different strings will do:
 *
 * on           Enable the user: it is possible to authenticate as this user.
 * off          Disable the user: it's no longer possible to authenticate
 *              with this user, however the already authenticated connections
 *              will still work.
 * +<command>   Allow the execution of that command.
 *              May be used with `|` for allowing subcommands (e.g "+config|get")
 * -<command>   Disallow the execution of that command.
 *              May be used with `|` for blocking subcommands (e.g "-config|set")
 * +@<category> Allow the execution of all the commands in such category
 *              with valid categories are like @admin, @set, @sortedset, ...
 *              and so forth, see the full list in the server.c file where
 *              the Redis command table is described and defined.
 *              The special category @all means all the commands, but currently
 *              present in the server, and that will be loaded in the future
 *              via modules.
 * +<command>|first-arg    Allow a specific first argument of an otherwise
 *                         disabled command. Note that this form is not
 *                         allowed as negative like -SELECT|1, but
 *                         only additive starting with "+".
 * allcommands  Alias for +@all. Note that it implies the ability to execute
 *              all the future commands loaded via the modules system.
 * nocommands   Alias for -@all.
 * ~<pattern>   Add a pattern of keys that can be mentioned as part of
 *              commands. For instance ~* allows all the keys. The pattern
 *              is a glob-style pattern like the one of KEYS.
 *              It is possible to specify multiple patterns.
 * allkeys      Alias for ~*
 * resetkeys    Flush the list of allowed keys patterns.
 * &<pattern>   Add a pattern of channels that can be mentioned as part of
 *              Pub/Sub commands. For instance &* allows all the channels. The
 *              pattern is a glob-style pattern like the one of PSUBSCRIBE.
 *              It is possible to specify multiple patterns.
 * allchannels              Alias for &*
 * resetchannels            Flush the list of allowed keys patterns.
 * ><password>  Add this password to the list of valid password for the user.
 *              For example >mypass will add "mypass" to the list.
 *              This directive clears the "nopass" flag (see later).
 * #<hash>      Add this password hash to the list of valid hashes for
 *              the user. This is useful if you have previously computed
 *              the hash, and don't want to store it in plaintext.
 *              This directive clears the "nopass" flag (see later).
 * <<password>  Remove this password from the list of valid passwords.
 * !<hash>      Remove this hashed password from the list of valid passwords.
 *              This is useful when you want to remove a password just by
 *              hash without knowing its plaintext version at all.
 * nopass       All the set passwords of the user are removed, and the user
 *              is flagged as requiring no password: it means that every
 *              password will work against this user. If this directive is
 *              used for the default user, every new connection will be
 *              immediately authenticated with the default user without
 *              any explicit AUTH command required. Note that the "resetpass"
 *              directive will clear this condition.
 * resetpass    Flush the list of allowed passwords. Moreover removes the
 *              "nopass" status. After "resetpass" the user has no associated
 *              passwords and there is no way to authenticate without adding
 *              some password (or setting it as "nopass" later).
 * reset        Performs the following actions: resetpass, resetkeys, off,
 *              -@all. The user returns to the same state it has immediately
 *              after its creation.
 *
 * The 'op' string must be null terminated. The 'oplen' argument should
 * specify the length of the 'op' string in case the caller requires to pass
 * binary data (for instance the >password form may use a binary password).
 * Otherwise the field can be set to -1 and the function will use strlen()
 * to determine the length.
 *
 * The function returns C_OK if the action to perform was understood because
 * the 'op' string made sense. Otherwise C_ERR is returned if the operation
 * is unknown or has some syntax error.
 *
 * When an error is returned, errno is set to the following values:
 *
 * EINVAL: The specified opcode is not understood or the key/channel pattern is
 *         invalid (contains non allowed characters).
 * ENOENT: The command name or command category provided with + or - is not
 *         known.
 * EEXIST: You are adding a key pattern after "*" was already added. This is
 *         almost surely an error on the user side.
 * EISDIR: You are adding a channel pattern after "*" was already added. This is
 *         almost surely an error on the user side.
 * ENODEV: The password you are trying to remove from the user does not exist.
 * EBADMSG: The hash you are trying to add is not a valid hash.
 */
int ACLSetUser(user *u, const char *op, ssize_t oplen) {
    if (oplen == -1) oplen = strlen(op);
    if (oplen == 0) return C_OK; /* Empty string is a no-operation. */
    if (!strcasecmp(op,"on")) {
        u->flags |= USER_FLAG_ENABLED;
        u->flags &= ~USER_FLAG_DISABLED;
    } else if (!strcasecmp(op,"off")) {
        u->flags |= USER_FLAG_DISABLED;
        u->flags &= ~USER_FLAG_ENABLED;
    } else if (!strcasecmp(op,"skip-sanitize-payload")) {
        u->flags |= USER_FLAG_SANITIZE_PAYLOAD_SKIP;
        u->flags &= ~USER_FLAG_SANITIZE_PAYLOAD;
    } else if (!strcasecmp(op,"sanitize-payload")) {
        u->flags &= ~USER_FLAG_SANITIZE_PAYLOAD_SKIP;
        u->flags |= USER_FLAG_SANITIZE_PAYLOAD;
    } else if (!strcasecmp(op,"allkeys") ||
               !strcasecmp(op,"~*"))
    {
        u->flags |= USER_FLAG_ALLKEYS;
        listEmpty(u->patterns);
    } else if (!strcasecmp(op,"resetkeys")) {
        u->flags &= ~USER_FLAG_ALLKEYS;
        listEmpty(u->patterns);
    } else if (!strcasecmp(op,"allchannels") ||
               !strcasecmp(op,"&*"))
    {
        u->flags |= USER_FLAG_ALLCHANNELS;
        listEmpty(u->channels);
    } else if (!strcasecmp(op,"resetchannels")) {
        u->flags &= ~USER_FLAG_ALLCHANNELS;
        listEmpty(u->channels);
    } else if (!strcasecmp(op,"allcommands") ||
               !strcasecmp(op,"+@all"))
    {
        memset(u->allowed_commands,255,sizeof(u->allowed_commands));
        u->flags |= USER_FLAG_ALLCOMMANDS;
        ACLResetFirstArgs(u);
    } else if (!strcasecmp(op,"nocommands") ||
               !strcasecmp(op,"-@all"))
    {
        memset(u->allowed_commands,0,sizeof(u->allowed_commands));
        u->flags &= ~USER_FLAG_ALLCOMMANDS;
        ACLResetFirstArgs(u);
    } else if (!strcasecmp(op,"nopass")) {
        u->flags |= USER_FLAG_NOPASS;
        listEmpty(u->passwords);
    } else if (!strcasecmp(op,"resetpass")) {
        u->flags &= ~USER_FLAG_NOPASS;
        listEmpty(u->passwords);
    } else if (op[0] == '>' || op[0] == '#') {
        sds newpass;
        if (op[0] == '>') {
            newpass = ACLHashPassword((unsigned char*)op+1,oplen-1);
        } else {
            if (ACLCheckPasswordHash((unsigned char*)op+1,oplen-1) == C_ERR) {
                errno = EBADMSG;
                return C_ERR;
            }
            newpass = sdsnewlen(op+1,oplen-1);
        }

        listNode *ln = listSearchKey(u->passwords,newpass);
        /* Avoid re-adding the same password multiple times. */
        if (ln == NULL)
            listAddNodeTail(u->passwords,newpass);
        else
            sdsfree(newpass);
        u->flags &= ~USER_FLAG_NOPASS;
    } else if (op[0] == '<' || op[0] == '!') {
        sds delpass;
        if (op[0] == '<') {
            delpass = ACLHashPassword((unsigned char*)op+1,oplen-1);
        } else {
            if (ACLCheckPasswordHash((unsigned char*)op+1,oplen-1) == C_ERR) {
                errno = EBADMSG;
                return C_ERR;
            }
            delpass = sdsnewlen(op+1,oplen-1);
        }
        listNode *ln = listSearchKey(u->passwords,delpass);
        sdsfree(delpass);
        if (ln) {
            listDelNode(u->passwords,ln);
        } else {
            errno = ENODEV;
            return C_ERR;
        }
    } else if (op[0] == '~') {
        if (u->flags & USER_FLAG_ALLKEYS) {
            errno = EEXIST;
            return C_ERR;
        }
        if (ACLStringHasSpaces(op+1,oplen-1)) {
            errno = EINVAL;
            return C_ERR;
        }
        sds newpat = sdsnewlen(op+1,oplen-1);
        listNode *ln = listSearchKey(u->patterns,newpat);
        /* Avoid re-adding the same key pattern multiple times. */
        if (ln == NULL)
            listAddNodeTail(u->patterns,newpat);
        else
            sdsfree(newpat);
        u->flags &= ~USER_FLAG_ALLKEYS;
    } else if (op[0] == '&') {
        if (u->flags & USER_FLAG_ALLCHANNELS) {
            errno = EISDIR;
            return C_ERR;
        }
        if (ACLStringHasSpaces(op+1,oplen-1)) {
            errno = EINVAL;
            return C_ERR;
        }
        sds newpat = sdsnewlen(op+1,oplen-1);
        listNode *ln = listSearchKey(u->channels,newpat);
        /* Avoid re-adding the same channel pattern multiple times. */
        if (ln == NULL)
            listAddNodeTail(u->channels,newpat);
        else
            sdsfree(newpat);
        u->flags &= ~USER_FLAG_ALLCHANNELS;
    } else if (op[0] == '+' && op[1] != '@') {
        if (strrchr(op,'|') == NULL) {
            struct redisCommand *cmd = ACLLookupCommand(op+1);
            if (cmd == NULL) {
                errno = ENOENT;
                return C_ERR;
            }
            ACLChangeCommandPerm(u,cmd,1);
        } else {
            /* Split the command and subcommand parts. */
            char *copy = zstrdup(op+1);
            char *sub = strrchr(copy,'|');
            sub[0] = '\0';
            sub++;

            struct redisCommand *cmd = ACLLookupCommand(copy);

            /* Check if the command exists. We can't check the
             * subcommand to see if it is valid. */
            if (cmd == NULL) {
                zfree(copy);
                errno = ENOENT;
                return C_ERR;
            }

            /* The subcommand cannot be empty, so things like DEBUG|
             * are syntax errors of course. */
            if (strlen(sub) == 0) {
                zfree(copy);
                errno = EINVAL;
                return C_ERR;
            }

            if (cmd->subcommands_dict) {
                /* If user is trying to allow a valid subcommand we can just add its unique ID */
                struct redisCommand *cmd = ACLLookupCommand(op+1);
                if (cmd == NULL) {
                    zfree(copy);
                    errno = ENOENT;
                    return C_ERR;
                }
                ACLChangeCommandPerm(u,cmd,1);
            } else {
                /* If user is trying to use the ACL mech to block SELECT except SELECT 0 or
                 * block DEBUG except DEBUG OBJECT (DEBUG subcommands are not considered
                 * subcommands for now) we use the allowed_firstargs mechanism. */
                struct redisCommand *cmd = ACLLookupCommand(copy);
                if (cmd == NULL) {
                    zfree(copy);
                    errno = ENOENT;
                    return C_ERR;
                }
                /* Add the first-arg to the list of valid ones. */
                ACLAddAllowedFirstArg(u,cmd->id,sub);
            }

            zfree(copy);
        }
    } else if (op[0] == '-' && op[1] != '@') {
        struct redisCommand *cmd = ACLLookupCommand(op+1);
        if (cmd == NULL) {
            errno = ENOENT;
            return C_ERR;
        }
        ACLChangeCommandPerm(u,cmd,0);
    } else if ((op[0] == '+' || op[0] == '-') && op[1] == '@') {
        int bitval = op[0] == '+' ? 1 : 0;
        if (ACLSetUserCommandBitsForCategory(u,op+2,bitval) == C_ERR) {
            errno = ENOENT;
            return C_ERR;
        }
    } else if (!strcasecmp(op,"reset")) {
        serverAssert(ACLSetUser(u,"resetpass",-1) == C_OK);
        serverAssert(ACLSetUser(u,"resetkeys",-1) == C_OK);
        serverAssert(ACLSetUser(u,"resetchannels",-1) == C_OK);
        if (server.acl_pubsub_default & USER_FLAG_ALLCHANNELS)
            serverAssert(ACLSetUser(u,"allchannels",-1) == C_OK);
        serverAssert(ACLSetUser(u,"off",-1) == C_OK);
        serverAssert(ACLSetUser(u,"sanitize-payload",-1) == C_OK);
        serverAssert(ACLSetUser(u,"-@all",-1) == C_OK);
    } else {
        errno = EINVAL;
        return C_ERR;
    }
    return C_OK;
}

/* Return a description of the error that occurred in ACLSetUser() according to
 * the errno value set by the function on error. */
const char *ACLSetUserStringError(void) {
    const char *errmsg = "Wrong format";
    if (errno == ENOENT)
        errmsg = "Unknown command or category name in ACL";
    else if (errno == EINVAL)
        errmsg = "Syntax error";
    else if (errno == EEXIST)
        errmsg = "Adding a pattern after the * pattern (or the "
                 "'allkeys' flag) is not valid and does not have any "
                 "effect. Try 'resetkeys' to start with an empty "
                 "list of patterns";
    else if (errno == EISDIR)
        errmsg = "Adding a pattern after the * pattern (or the "
                 "'allchannels' flag) is not valid and does not have any "
                 "effect. Try 'resetchannels' to start with an empty "
                 "list of channels";
    else if (errno == ENODEV)
        errmsg = "The password you are trying to remove from the user does "
                 "not exist";
    else if (errno == EBADMSG)
        errmsg = "The password hash must be exactly 64 characters and contain "
                 "only lowercase hexadecimal characters";
    else if (errno == EALREADY)
        errmsg = "Duplicate user found. A user can only be defined once in "
                 "config files";
    return errmsg;
}

/* Create the default user, this has special permissions. */
user *ACLCreateDefaultUser(void) {
    user *new = ACLCreateUser("default",7);
    ACLSetUser(new,"+@all",-1);
    ACLSetUser(new,"~*",-1);
    ACLSetUser(new,"&*",-1);
    ACLSetUser(new,"on",-1);
    ACLSetUser(new,"nopass",-1);
    return new;
}

/* Initialization of the ACL subsystem. */
void ACLInit(void) {
    Users = raxNew();
    UsersToLoad = listCreate();
    listSetMatchMethod(UsersToLoad, ACLListMatchLoadedUser);
    ACLLog = listCreate();
    DefaultUser = ACLCreateDefaultUser();
}

/* Check the username and password pair and return C_OK if they are valid,
 * otherwise C_ERR is returned and errno is set to:
 *
 *  EINVAL: if the username-password do not match.
 *  ENONENT: if the specified user does not exist at all.
 */
int ACLCheckUserCredentials(robj *username, robj *password) {
    user *u = ACLGetUserByName(username->ptr,sdslen(username->ptr));
    if (u == NULL) {
        errno = ENOENT;
        return C_ERR;
    }

    /* Disabled users can't login. */
    if (u->flags & USER_FLAG_DISABLED) {
        errno = EINVAL;
        return C_ERR;
    }

    /* If the user is configured to don't require any password, we
     * are already fine here. */
    if (u->flags & USER_FLAG_NOPASS) return C_OK;

    /* Check all the user passwords for at least one to match. */
    listIter li;
    listNode *ln;
    listRewind(u->passwords,&li);
    sds hashed = ACLHashPassword(password->ptr,sdslen(password->ptr));
    while((ln = listNext(&li))) {
        sds thispass = listNodeValue(ln);
        if (!time_independent_strcmp(hashed, thispass)) {
            sdsfree(hashed);
            return C_OK;
        }
    }
    sdsfree(hashed);

    /* If we reached this point, no password matched. */
    errno = EINVAL;
    return C_ERR;
}

/* This is like ACLCheckUserCredentials(), however if the user/pass
 * are correct, the connection is put in authenticated state and the
 * connection user reference is populated.
 *
 * The return value is C_OK or C_ERR with the same meaning as
 * ACLCheckUserCredentials(). */
int ACLAuthenticateUser(client *c, robj *username, robj *password) {
    if (ACLCheckUserCredentials(username,password) == C_OK) {
        c->authenticated = 1;
        c->user = ACLGetUserByName(username->ptr,sdslen(username->ptr));
        moduleNotifyUserChanged(c);
        return C_OK;
    } else {
        addACLLogEntry(c,ACL_DENIED_AUTH,(c->flags & CLIENT_MULTI) ? ACL_LOG_CTX_MULTI : ACL_LOG_CTX_TOPLEVEL,0,username->ptr,NULL);
        return C_ERR;
    }
}

/* For ACL purposes, every user has a bitmap with the commands that such
 * user is allowed to execute. In order to populate the bitmap, every command
 * should have an assigned ID (that is used to index the bitmap). This function
 * creates such an ID: it uses sequential IDs, reusing the same ID for the same
 * command name, so that a command retains the same ID in case of modules that
 * are unloaded and later reloaded. */
unsigned long ACLGetCommandID(const char *cmdname) {
    sds lowername = sdsnew(cmdname);
    sdstolower(lowername);
    if (commandId == NULL) commandId = raxNew();
    void *id = raxFind(commandId,(unsigned char*)lowername,sdslen(lowername));
    if (id != raxNotFound) {
        sdsfree(lowername);
        return (unsigned long)id;
    }
    raxInsert(commandId,(unsigned char*)lowername,strlen(lowername),
              (void*)nextid,NULL);
    sdsfree(lowername);
    unsigned long thisid = nextid;
    nextid++;

    /* We never assign the last bit in the user commands bitmap structure,
     * this way we can later check if this bit is set, understanding if the
     * current ACL for the user was created starting with a +@all to add all
     * the possible commands and just subtracting other single commands or
     * categories, or if, instead, the ACL was created just adding commands
     * and command categories from scratch, not allowing future commands by
     * default (loaded via modules). This is useful when rewriting the ACLs
     * with ACL SAVE. */
    if (nextid == USER_COMMAND_BITS_COUNT-1) nextid++;
    return thisid;
}

/* Clear command id table and reset nextid to 0. */
void ACLClearCommandID(void) {
    if (commandId) raxFree(commandId);
    commandId = NULL;
    nextid = 0;
}

/* Return an username by its name, or NULL if the user does not exist. */
user *ACLGetUserByName(const char *name, size_t namelen) {
    void *myuser = raxFind(Users,(unsigned char*)name,namelen);
    if (myuser == raxNotFound) return NULL;
    return myuser;
}

/* Check if the key can be accessed by the client according to
 * the ACLs associated with the specified user.
 *
 * If the user can access the key, ACL_OK is returned, otherwise
 * ACL_DENIED_KEY is returned. */
int ACLCheckKey(const user *u, const char *key, int keylen) {
    /* If there is no associated user, the connection can run anything. */
    if (u == NULL) return ACL_OK;

    /* The user can run any keys */
    if (u->flags & USER_FLAG_ALLKEYS) return ACL_OK;

    listIter li;
    listNode *ln;
    listRewind(u->patterns,&li);

    /* Test this key against every pattern. */
    while((ln = listNext(&li))) {
        sds pattern = listNodeValue(ln);
        size_t plen = sdslen(pattern);
        if (stringmatchlen(pattern,plen,key,keylen,0))
            return ACL_OK;
    }
    return ACL_DENIED_KEY;
}

/* Check if the command is ready to be executed according to the
 * ACLs associated with the specified user.
 *
 * If the user can execute the command ACL_OK is returned, otherwise
 * ACL_DENIED_CMD or ACL_DENIED_KEY is returned: the first in case the
 * command cannot be executed because the user is not allowed to run such
 * command, the second if the command is denied because the user is trying
 * to access keys that are not among the specified patterns. */
int ACLCheckCommandPerm(const user *u, struct redisCommand *cmd, robj **argv, int argc, int *keyidxptr) {
    int ret;
    uint64_t id = cmd->id;

    /* If there is no associated user, the connection can run anything. */
    if (u == NULL) return ACL_OK;

    /* Check if the user can execute this command or if the command
     * doesn't need to be authenticated (hello, auth). */
    if (!(u->flags & USER_FLAG_ALLCOMMANDS) && !(cmd->flags & CMD_NO_AUTH))
    {
        /* If the bit is not set we have to check further, in case the
         * command is allowed just with that specific first argument. */
        if (ACLGetUserCommandBit(u,id) == 0) {
            /* Check if the first argument matches. */
            if (argc < 2 ||
                u->allowed_firstargs == NULL ||
                u->allowed_firstargs[id] == NULL)
            {
                return ACL_DENIED_CMD;
            }

            long subid = 0;
            while (1) {
                if (u->allowed_firstargs[id][subid] == NULL)
                    return ACL_DENIED_CMD;
                int idx = cmd->parent ? 2 : 1;
                if (!strcasecmp(argv[idx]->ptr,u->allowed_firstargs[id][subid]))
                    break; /* First argument match found. Stop here. */
                subid++;
            }
        }
    }

    /* Check if the user can execute commands explicitly touching the keys
     * mentioned in the command arguments. */
    if (!(u->flags & USER_FLAG_ALLKEYS) &&
        (cmd->getkeys_proc || cmd->key_specs_num))
    {
        getKeysResult result = GETKEYS_RESULT_INIT;
        int numkeys = getKeysFromCommand(cmd,argv,argc,&result);
        int *keyidx = result.keys;
        for (int j = 0; j < numkeys; j++) {
            int idx = keyidx[j];
            ret = ACLCheckKey(u, argv[idx]->ptr, sdslen(argv[idx]->ptr));
            if (ret != ACL_OK) {
                if (keyidxptr) *keyidxptr = keyidx[j];
                getKeysFreeResult(&result);
                return ret;
            }
        }
        getKeysFreeResult(&result);
    }

    /* If we survived all the above checks, the user can execute the
     * command. */
    return ACL_OK;
}

/* Check if the provided channel is whitelisted by the given allowed channels
 * list. Glob-style pattern matching is employed, unless the literal flag is
 * set. Returns ACL_OK if access is granted or ACL_DENIED_CHANNEL otherwise. */
int ACLCheckPubsubChannelPerm(sds channel, list *allowed, int literal) {
    listIter li;
    listNode *ln;
    size_t clen = sdslen(channel);
    int match = 0;

    listRewind(allowed,&li);
    while((ln = listNext(&li))) {
        sds pattern = listNodeValue(ln);
        size_t plen = sdslen(pattern);

        if ((literal && !sdscmp(pattern,channel)) || 
            (!literal && stringmatchlen(pattern,plen,channel,clen,0)))
        {
            match = 1;
            break;
        }
    }
    if (!match) {
        return ACL_DENIED_CHANNEL;
    }
    return ACL_OK;
}

/* Check if the user's existing pub/sub clients violate the ACL pub/sub
 * permissions specified via the upcoming argument, and kill them if so. */
void ACLKillPubsubClientsIfNeeded(user *u, list *upcoming) {
    listIter li, lpi;
    listNode *ln, *lpn;
    robj *o;
    int kill = 0;
    
    /* Nothing to kill when the upcoming are a literal super set of the original
     * permissions. */
    listRewind(u->channels,&li);
    while (!kill && ((ln = listNext(&li)) != NULL)) {
        sds pattern = listNodeValue(ln);
        kill = (ACLCheckPubsubChannelPerm(pattern,upcoming,1) ==
                ACL_DENIED_CHANNEL);
    }
    if (!kill) return;

    /* Scan all connected clients to find the user's pub/subs. */
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        kill = 0;

        if (c->user == u && getClientType(c) == CLIENT_TYPE_PUBSUB) {
            /* Check for pattern violations. */
            listRewind(c->pubsub_patterns,&lpi);
            while (!kill && ((lpn = listNext(&lpi)) != NULL)) {
                o = lpn->value;
                kill = (ACLCheckPubsubChannelPerm(o->ptr,upcoming,1) == 
                        ACL_DENIED_CHANNEL);
            }
            /* Check for channel violations. */
            if (!kill) {
                dictIterator *di = dictGetIterator(c->pubsub_channels);
                dictEntry *de;                
                while (!kill && ((de = dictNext(di)) != NULL)) {
                    o = dictGetKey(de);
                    kill = (ACLCheckPubsubChannelPerm(o->ptr,upcoming,0) ==
                            ACL_DENIED_CHANNEL);
                }
                dictReleaseIterator(di);
            }

            /* Kill it. */
            if (kill) {
                freeClient(c);
            }
        }
    }
}

/* Check if the pub/sub channels of the command, that's ready to be executed
 * according to the ACLs channels associated with the specified user.
 * 
 * idx and count are the index and count of channel arguments from the
 * command. The literal argument controls whether the user's ACL channels are 
 * evaluated as literal values or matched as glob-like patterns.
 *
 * If the user can execute the command ACL_OK is returned, otherwise 
 * ACL_DENIED_CHANNEL. */
int ACLCheckPubsubPerm(const user *u, robj **argv, int idx, int count, int literal, int *idxptr) {
    /* If there is no associated user, the connection can run anything. */
    if (u == NULL) return ACL_OK;

    /* Check if the user can access the channels mentioned in the command's
     * arguments. */
    if (!(u->flags & USER_FLAG_ALLCHANNELS)) {
        for (int j = idx; j < idx+count; j++) {
            if (ACLCheckPubsubChannelPerm(argv[j]->ptr,u->channels,literal)
                != ACL_OK) {
                if (idxptr) *idxptr = j;
                return ACL_DENIED_CHANNEL;
            }
        }
    }

    /* If we survived all the above checks, the user can execute the
     * command. */
    return ACL_OK;

}

/* Check whether the command is ready to be executed by ACLCheckCommandPerm.
 * If check passes, then check whether pub/sub channels of the command is
 * ready to be executed by ACLCheckPubsubPerm */
int ACLCheckAllUserCommandPerm(const user *u, struct redisCommand *cmd, robj **argv, int argc, int *idxptr) {
    int acl_retval = ACLCheckCommandPerm(u,cmd,argv,argc,idxptr);
    if (acl_retval != ACL_OK)
        return acl_retval;
    if (cmd->proc == publishCommand)
        acl_retval = ACLCheckPubsubPerm(u,argv,1,1,0,idxptr);
    else if (cmd->proc == subscribeCommand)
        acl_retval = ACLCheckPubsubPerm(u,argv,1,argc-1,0,idxptr);
    else if (cmd->proc == psubscribeCommand)
        acl_retval = ACLCheckPubsubPerm(u,argv,1,argc-1,1,idxptr);
    return acl_retval;
}

int ACLCheckAllPerm(client *c, int *idxptr) {
    return ACLCheckAllUserCommandPerm(c->user, c->cmd, c->argv, c->argc, idxptr);
}

/* =============================================================================
 * ACL loading / saving functions
 * ==========================================================================*/

/* Given an argument vector describing a user in the form:
 *
 *      user <username> ... ACL rules and flags ...
 *
 * this function validates, and if the syntax is valid, appends
 * the user definition to a list for later loading.
 *
 * The rules are tested for validity and if there obvious syntax errors
 * the function returns C_ERR and does nothing, otherwise C_OK is returned
 * and the user is appended to the list.
 *
 * Note that this function cannot stop in case of commands that are not found
 * and, in that case, the error will be emitted later, because certain
 * commands may be defined later once modules are loaded.
 *
 * When an error is detected and C_ERR is returned, the function populates
 * by reference (if not set to NULL) the argc_err argument with the index
 * of the argv vector that caused the error. */
int ACLAppendUserForLoading(sds *argv, int argc, int *argc_err) {
    if (argc < 2 || strcasecmp(argv[0],"user")) {
        if (argc_err) *argc_err = 0;
        return C_ERR;
    }

    if (listSearchKey(UsersToLoad, argv[1])) {
        if (argc_err) *argc_err = 1;
        errno = EALREADY;
        return C_ERR; 
    }

    /* Try to apply the user rules in a fake user to see if they
     * are actually valid. */
    user *fakeuser = ACLCreateUnlinkedUser();

    for (int j = 2; j < argc; j++) {
        if (ACLSetUser(fakeuser,argv[j],sdslen(argv[j])) == C_ERR) {
            if (errno != ENOENT) {
                ACLFreeUser(fakeuser);
                if (argc_err) *argc_err = j;
                return C_ERR;
            }
        }
    }

    /* Rules look valid, let's append the user to the list. */
    sds *copy = zmalloc(sizeof(sds)*argc);
    for (int j = 1; j < argc; j++) copy[j-1] = sdsdup(argv[j]);
    copy[argc-1] = NULL;
    listAddNodeTail(UsersToLoad,copy);
    ACLFreeUser(fakeuser);
    return C_OK;
}

/* This function will load the configured users appended to the server
 * configuration via ACLAppendUserForLoading(). On loading errors it will
 * log an error and return C_ERR, otherwise C_OK will be returned. */
int ACLLoadConfiguredUsers(void) {
    listIter li;
    listNode *ln;
    listRewind(UsersToLoad,&li);
    while ((ln = listNext(&li)) != NULL) {
        sds *aclrules = listNodeValue(ln);
        sds username = aclrules[0];

        if (ACLStringHasSpaces(aclrules[0],sdslen(aclrules[0]))) {
            serverLog(LL_WARNING,"Spaces not allowed in ACL usernames");
            return C_ERR;
        }

        user *u = ACLCreateUser(username,sdslen(username));
        if (!u) {
            /* Only valid duplicate user is the default one. */
            serverAssert(!strcmp(username, "default"));
            u = ACLGetUserByName("default",7);
            ACLSetUser(u,"reset",-1);
        }

        /* Load every rule defined for this user. */
        for (int j = 1; aclrules[j]; j++) {
            if (ACLSetUser(u,aclrules[j],sdslen(aclrules[j])) != C_OK) {
                const char *errmsg = ACLSetUserStringError();
                serverLog(LL_WARNING,"Error loading ACL rule '%s' for "
                                     "the user named '%s': %s",
                          aclrules[j],aclrules[0],errmsg);
                return C_ERR;
            }
        }

        /* Having a disabled user in the configuration may be an error,
         * warn about it without returning any error to the caller. */
        if (u->flags & USER_FLAG_DISABLED) {
            serverLog(LL_NOTICE, "The user '%s' is disabled (there is no "
                                 "'on' modifier in the user description). Make "
                                 "sure this is not a configuration error.",
                      aclrules[0]);
        }
    }
    return C_OK;
}

/* This function loads the ACL from the specified filename: every line
 * is validated and should be either empty or in the format used to specify
 * users in the redis.conf configuration or in the ACL file, that is:
 *
 *  user <username> ... rules ...
 *
 * Note that this function considers comments starting with '#' as errors
 * because the ACL file is meant to be rewritten, and comments would be
 * lost after the rewrite. Yet empty lines are allowed to avoid being too
 * strict.
 *
 * One important part of implementing ACL LOAD, that uses this function, is
 * to avoid ending with broken rules if the ACL file is invalid for some
 * reason, so the function will attempt to validate the rules before loading
 * each user. For every line that will be found broken the function will
 * collect an error message.
 *
 * IMPORTANT: If there is at least a single error, nothing will be loaded
 * and the rules will remain exactly as they were.
 *
 * At the end of the process, if no errors were found in the whole file then
 * NULL is returned. Otherwise an SDS string describing in a single line
 * a description of all the issues found is returned. */
sds ACLLoadFromFile(const char *filename) {
    FILE *fp;
    char buf[1024];

    /* Open the ACL file. */
    if ((fp = fopen(filename,"r")) == NULL) {
        sds errors = sdscatprintf(sdsempty(),
            "Error loading ACLs, opening file '%s': %s",
            filename, strerror(errno));
        return errors;
    }

    /* Load the whole file as a single string in memory. */
    sds acls = sdsempty();
    while(fgets(buf,sizeof(buf),fp) != NULL)
        acls = sdscat(acls,buf);
    fclose(fp);

    /* Split the file into lines and attempt to load each line. */
    int totlines;
    sds *lines, errors = sdsempty();
    lines = sdssplitlen(acls,strlen(acls),"\n",1,&totlines);
    sdsfree(acls);

    /* We do all the loading in a fresh instance of the Users radix tree,
     * so if there are errors loading the ACL file we can rollback to the
     * old version. */
    rax *old_users = Users;
    Users = raxNew();

    /* Load each line of the file. */
    for (int i = 0; i < totlines; i++) {
        sds *argv;
        int argc;
        int linenum = i+1;

        lines[i] = sdstrim(lines[i]," \t\r\n");

        /* Skip blank lines */
        if (lines[i][0] == '\0') continue;

        /* Split into arguments */
        argv = sdssplitlen(lines[i],sdslen(lines[i])," ",1,&argc);
        if (argv == NULL) {
            errors = sdscatprintf(errors,
                     "%s:%d: unbalanced quotes in acl line. ",
                     server.acl_filename, linenum);
            continue;
        }

        /* Skip this line if the resulting command vector is empty. */
        if (argc == 0) {
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* The line should start with the "user" keyword. */
        if (strcmp(argv[0],"user") || argc < 2) {
            errors = sdscatprintf(errors,
                     "%s:%d should start with user keyword followed "
                     "by the username. ", server.acl_filename,
                     linenum);
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Spaces are not allowed in usernames. */
        if (ACLStringHasSpaces(argv[1],sdslen(argv[1]))) {
            errors = sdscatprintf(errors,
                     "'%s:%d: username '%s' contains invalid characters. ",
                     server.acl_filename, linenum, argv[1]);
            sdsfreesplitres(argv,argc);
            continue;
        }

        user *u = ACLCreateUser(argv[1],sdslen(argv[1]));

        /* If the user already exists we assume it's an error and abort. */
        if (!u) {
            errors = sdscatprintf(errors,"WARNING: Duplicate user '%s' found on line %d. ", argv[1], linenum);
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Finally process the options and validate they can
         * be cleanly applied to the user. If any option fails
         * to apply, the other values won't be applied since
         * all the pending changes will get dropped. */
        int j;
        for (j = 2; j < argc; j++) {
            argv[j] = sdstrim(argv[j],"\t\r\n");
            if (ACLSetUser(u,argv[j],sdslen(argv[j])) != C_OK) {
                const char *errmsg = ACLSetUserStringError();
                errors = sdscatprintf(errors,
                         "%s:%d: %s. ",
                         server.acl_filename, linenum, errmsg);
                continue;
            }
        }

        /* Apply the rule to the new users set only if so far there
         * are no errors, otherwise it's useless since we are going
         * to discard the new users set anyway. */
        if (sdslen(errors) != 0) {
            sdsfreesplitres(argv,argc);
            continue;
        }

        sdsfreesplitres(argv,argc);
    }

    sdsfreesplitres(lines,totlines);

    /* Check if we found errors and react accordingly. */
    if (sdslen(errors) == 0) {
        /* The default user pointer is referenced in different places: instead
         * of replacing such occurrences it is much simpler to copy the new
         * default user configuration in the old one. */
        user *new_default = ACLGetUserByName("default",7);
        if (!new_default) {
            new_default = ACLCreateDefaultUser();
        }

        ACLCopyUser(DefaultUser,new_default);
        ACLFreeUser(new_default);
        raxInsert(Users,(unsigned char*)"default",7,DefaultUser,NULL);
        raxRemove(old_users,(unsigned char*)"default",7,NULL);
        ACLFreeUsersSet(old_users);
        sdsfree(errors);
        return NULL;
    } else {
        ACLFreeUsersSet(Users);
        Users = old_users;
        errors = sdscat(errors,"WARNING: ACL errors detected, no change to the previously active ACL rules was performed");
        return errors;
    }
}

/* Generate a copy of the ACLs currently in memory in the specified filename.
 * Returns C_OK on success or C_ERR if there was an error during the I/O.
 * When C_ERR is returned a log is produced with hints about the issue. */
int ACLSaveToFile(const char *filename) {
    sds acl = sdsempty();
    int fd = -1;
    sds tmpfilename = NULL;
    int retval = C_ERR;

    /* Let's generate an SDS string containing the new version of the
     * ACL file. */
    raxIterator ri;
    raxStart(&ri,Users);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        user *u = ri.data;
        /* Return information in the configuration file format. */
        sds user = sdsnew("user ");
        user = sdscatsds(user,u->name);
        user = sdscatlen(user," ",1);
        sds descr = ACLDescribeUser(u);
        user = sdscatsds(user,descr);
        sdsfree(descr);
        acl = sdscatsds(acl,user);
        acl = sdscatlen(acl,"\n",1);
        sdsfree(user);
    }
    raxStop(&ri);

    /* Create a temp file with the new content. */
    tmpfilename = sdsnew(filename);
    tmpfilename = sdscatfmt(tmpfilename,".tmp-%i-%I",
        (int)getpid(),(int)mstime());
    if ((fd = open(tmpfilename,O_WRONLY|O_CREAT,0644)) == -1) {
        serverLog(LL_WARNING,"Opening temp ACL file for ACL SAVE: %s",
            strerror(errno));
        goto cleanup;
    }

    /* Write it. */
    if (write(fd,acl,sdslen(acl)) != (ssize_t)sdslen(acl)) {
        serverLog(LL_WARNING,"Writing ACL file for ACL SAVE: %s",
            strerror(errno));
        goto cleanup;
    }
    close(fd); fd = -1;

    /* Let's replace the new file with the old one. */
    if (rename(tmpfilename,filename) == -1) {
        serverLog(LL_WARNING,"Renaming ACL file for ACL SAVE: %s",
            strerror(errno));
        goto cleanup;
    }
    sdsfree(tmpfilename); tmpfilename = NULL;
    retval = C_OK; /* If we reached this point, everything is fine. */

cleanup:
    if (fd != -1) close(fd);
    if (tmpfilename) unlink(tmpfilename);
    sdsfree(tmpfilename);
    sdsfree(acl);
    return retval;
}

/* This function is called once the server is already running, modules are
 * loaded, and we are ready to start, in order to load the ACLs either from
 * the pending list of users defined in redis.conf, or from the ACL file.
 * The function will just exit with an error if the user is trying to mix
 * both the loading methods. */
void ACLLoadUsersAtStartup(void) {
    if (server.acl_filename[0] != '\0' && listLength(UsersToLoad) != 0) {
        serverLog(LL_WARNING,
            "Configuring Redis with users defined in redis.conf and at "
            "the same setting an ACL file path is invalid. This setup "
            "is very likely to lead to configuration errors and security "
            "holes, please define either an ACL file or declare users "
            "directly in your redis.conf, but not both.");
        exit(1);
    }

    if (ACLLoadConfiguredUsers() == C_ERR) {
        serverLog(LL_WARNING,
            "Critical error while loading ACLs. Exiting.");
        exit(1);
    }

    if (server.acl_filename[0] != '\0') {
        sds errors = ACLLoadFromFile(server.acl_filename);
        if (errors) {
            serverLog(LL_WARNING,
                "Aborting Redis startup because of ACL errors: %s", errors);
            sdsfree(errors);
            exit(1);
        }
    }
}

/* =============================================================================
 * ACL log
 * ==========================================================================*/

#define ACL_LOG_GROUPING_MAX_TIME_DELTA 60000

/* This structure defines an entry inside the ACL log. */
typedef struct ACLLogEntry {
    uint64_t count;     /* Number of times this happened recently. */
    int reason;         /* Reason for denying the command. ACL_DENIED_*. */
    int context;        /* Toplevel, Lua or MULTI/EXEC? ACL_LOG_CTX_*. */
    sds object;         /* The key name or command name. */
    sds username;       /* User the client is authenticated with. */
    mstime_t ctime;     /* Milliseconds time of last update to this entry. */
    sds cinfo;          /* Client info (last client if updated). */
} ACLLogEntry;

/* This function will check if ACL entries 'a' and 'b' are similar enough
 * that we should actually update the existing entry in our ACL log instead
 * of creating a new one. */
int ACLLogMatchEntry(ACLLogEntry *a, ACLLogEntry *b) {
    if (a->reason != b->reason) return 0;
    if (a->context != b->context) return 0;
    mstime_t delta = a->ctime - b->ctime;
    if (delta < 0) delta = -delta;
    if (delta > ACL_LOG_GROUPING_MAX_TIME_DELTA) return 0;
    if (sdscmp(a->object,b->object) != 0) return 0;
    if (sdscmp(a->username,b->username) != 0) return 0;
    return 1;
}

/* Release an ACL log entry. */
void ACLFreeLogEntry(void *leptr) {
    ACLLogEntry *le = leptr;
    sdsfree(le->object);
    sdsfree(le->username);
    sdsfree(le->cinfo);
    zfree(le);
}

/* Adds a new entry in the ACL log, making sure to delete the old entry
 * if we reach the maximum length allowed for the log. This function attempts
 * to find similar entries in the current log in order to bump the counter of
 * the log entry instead of creating many entries for very similar ACL
 * rules issues.
 *
 * The argpos argument is used when the reason is ACL_DENIED_KEY or 
 * ACL_DENIED_CHANNEL, since it allows the function to log the key or channel
 * name that caused the problem.
 *
 * The last 2 arguments are a manual override to be used, instead of any of the automatic
 * ones which depend on the client and reason arguments (use NULL for default).
 */
void addACLLogEntry(client *c, int reason, int context, int argpos, sds username, sds object) {
    /* Create a new entry. */
    struct ACLLogEntry *le = zmalloc(sizeof(*le));
    le->count = 1;
    le->reason = reason;
    le->username = sdsdup(username ? username : c->user->name);
    le->ctime = mstime();

    if (object) {
        le->object = sdsnew(object);
    } else {
        switch(reason) {
            case ACL_DENIED_CMD: le->object = sdsnew(c->cmd->name); break;
            case ACL_DENIED_KEY: le->object = sdsdup(c->argv[argpos]->ptr); break;
            case ACL_DENIED_CHANNEL: le->object = sdsdup(c->argv[argpos]->ptr); break;
            case ACL_DENIED_AUTH: le->object = sdsdup(c->argv[0]->ptr); break;
            default: le->object = sdsempty();
        }
    }

    client *realclient = c;
    if (realclient->flags & CLIENT_SCRIPT) realclient = server.script_caller;

    le->cinfo = catClientInfoString(sdsempty(),realclient);
    le->context = context;

    /* Try to match this entry with past ones, to see if we can just
     * update an existing entry instead of creating a new one. */
    long toscan = 10; /* Do a limited work trying to find duplicated. */
    listIter li;
    listNode *ln;
    listRewind(ACLLog,&li);
    ACLLogEntry *match = NULL;
    while (toscan-- && (ln = listNext(&li)) != NULL) {
        ACLLogEntry *current = listNodeValue(ln);
        if (ACLLogMatchEntry(current,le)) {
            match = current;
            listDelNode(ACLLog,ln);
            listAddNodeHead(ACLLog,current);
            break;
        }
    }

    /* If there is a match update the entry, otherwise add it as a
     * new one. */
    if (match) {
        /* We update a few fields of the existing entry and bump the
         * counter of events for this entry. */
        sdsfree(match->cinfo);
        match->cinfo = le->cinfo;
        match->ctime = le->ctime;
        match->count++;

        /* Release the old entry. */
        le->cinfo = NULL;
        ACLFreeLogEntry(le);
    } else {
        /* Add it to our list of entries. We'll have to trim the list
         * to its maximum size. */
        listAddNodeHead(ACLLog, le);
        while(listLength(ACLLog) > server.acllog_max_len) {
            listNode *ln = listLast(ACLLog);
            ACLLogEntry *le = listNodeValue(ln);
            ACLFreeLogEntry(le);
            listDelNode(ACLLog,ln);
        }
    }
}

/* =============================================================================
 * ACL related commands
 * ==========================================================================*/

/* ACL -- show and modify the configuration of ACL users.
 * ACL HELP
 * ACL LOAD
 * ACL SAVE
 * ACL LIST
 * ACL USERS
 * ACL CAT [<category>]
 * ACL SETUSER <username> ... acl rules ...
 * ACL DELUSER <username> [...]
 * ACL GETUSER <username>
 * ACL GENPASS [<bits>]
 * ACL WHOAMI
 * ACL LOG [<count> | RESET]
 */
void aclCommand(client *c) {
    char *sub = c->argv[1]->ptr;
    if (!strcasecmp(sub,"setuser") && c->argc >= 3) {
        /* Initially redact all of the arguments to not leak any information
         * about the user. */
        for (int j = 2; j < c->argc; j++) {
            redactClientCommandArgument(c, j);
        }

        sds username = c->argv[2]->ptr;
        /* Check username validity. */
        if (ACLStringHasSpaces(username,sdslen(username))) {
            addReplyErrorFormat(c,
                "Usernames can't contain spaces or null characters");
            return;
        }

        /* Create a temporary user to validate and stage all changes against
         * before applying to an existing user or creating a new user. If all
         * arguments are valid the user parameters will all be applied together.
         * If there are any errors then none of the changes will be applied. */
        user *tempu = ACLCreateUnlinkedUser();
        user *u = ACLGetUserByName(username,sdslen(username));
        if (u) ACLCopyUser(tempu, u);

        for (int j = 3; j < c->argc; j++) {
            if (ACLSetUser(tempu,c->argv[j]->ptr,sdslen(c->argv[j]->ptr)) != C_OK) {
                const char *errmsg = ACLSetUserStringError();
                addReplyErrorFormat(c,
                    "Error in ACL SETUSER modifier '%s': %s",
                    (char*)c->argv[j]->ptr, errmsg);

                ACLFreeUser(tempu);
                return;
            }
        }

        /* Existing pub/sub clients authenticated with the user may need to be
         * disconnected if (some of) their channel permissions were revoked. */
        if (u && !(tempu->flags & USER_FLAG_ALLCHANNELS))
            ACLKillPubsubClientsIfNeeded(u,tempu->channels);

        /* Overwrite the user with the temporary user we modified above. */
        if (!u) u = ACLCreateUser(username,sdslen(username));
        serverAssert(u != NULL);
        ACLCopyUser(u, tempu);
        ACLFreeUser(tempu);
        addReply(c,shared.ok);
    } else if (!strcasecmp(sub,"deluser") && c->argc >= 3) {
        int deleted = 0;
        for (int j = 2; j < c->argc; j++) {
            sds username = c->argv[j]->ptr;
            if (!strcmp(username,"default")) {
                addReplyError(c,"The 'default' user cannot be removed");
                return;
            }
        }

        for (int j = 2; j < c->argc; j++) {
            sds username = c->argv[j]->ptr;
            user *u;
            if (raxRemove(Users,(unsigned char*)username,
                          sdslen(username),
                          (void**)&u))
            {
                ACLFreeUserAndKillClients(u);
                deleted++;
            }
        }
        addReplyLongLong(c,deleted);
    } else if (!strcasecmp(sub,"getuser") && c->argc == 3) {
        user *u = ACLGetUserByName(c->argv[2]->ptr,sdslen(c->argv[2]->ptr));
        if (u == NULL) {
            addReplyNull(c);
            return;
        }

        addReplyMapLen(c,5);

        /* Flags */
        addReplyBulkCString(c,"flags");
        void *deflen = addReplyDeferredLen(c);
        int numflags = 0;
        for (int j = 0; ACLUserFlags[j].flag; j++) {
            if (u->flags & ACLUserFlags[j].flag) {
                addReplyBulkCString(c,ACLUserFlags[j].name);
                numflags++;
            }
        }
        setDeferredSetLen(c,deflen,numflags);

        /* Passwords */
        addReplyBulkCString(c,"passwords");
        addReplyArrayLen(c,listLength(u->passwords));
        listIter li;
        listNode *ln;
        listRewind(u->passwords,&li);
        while((ln = listNext(&li))) {
            sds thispass = listNodeValue(ln);
            addReplyBulkCBuffer(c,thispass,sdslen(thispass));
        }

        /* Commands */
        addReplyBulkCString(c,"commands");
        sds cmddescr = ACLDescribeUserCommandRules(u);
        addReplyBulkSds(c,cmddescr);

        /* Key patterns */
        addReplyBulkCString(c,"keys");
        if (u->flags & USER_FLAG_ALLKEYS) {
            addReplyArrayLen(c,1);
            addReplyBulkCBuffer(c,"*",1);
        } else {
            addReplyArrayLen(c,listLength(u->patterns));
            listIter li;
            listNode *ln;
            listRewind(u->patterns,&li);
            while((ln = listNext(&li))) {
                sds thispat = listNodeValue(ln);
                addReplyBulkCBuffer(c,thispat,sdslen(thispat));
            }
        }

        /* Pub/sub patterns */
        addReplyBulkCString(c,"channels");
        if (u->flags & USER_FLAG_ALLCHANNELS) {
            addReplyArrayLen(c,1);
            addReplyBulkCBuffer(c,"*",1);
        } else {
            addReplyArrayLen(c,listLength(u->channels));
            listIter li;
            listNode *ln;
            listRewind(u->channels,&li);
            while((ln = listNext(&li))) {
                sds thispat = listNodeValue(ln);
                addReplyBulkCBuffer(c,thispat,sdslen(thispat));
            }
        }
    } else if ((!strcasecmp(sub,"list") || !strcasecmp(sub,"users")) &&
               c->argc == 2)
    {
        int justnames = !strcasecmp(sub,"users");
        addReplyArrayLen(c,raxSize(Users));
        raxIterator ri;
        raxStart(&ri,Users);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            user *u = ri.data;
            if (justnames) {
                addReplyBulkCBuffer(c,u->name,sdslen(u->name));
            } else {
                /* Return information in the configuration file format. */
                sds config = sdsnew("user ");
                config = sdscatsds(config,u->name);
                config = sdscatlen(config," ",1);
                sds descr = ACLDescribeUser(u);
                config = sdscatsds(config,descr);
                sdsfree(descr);
                addReplyBulkSds(c,config);
            }
        }
        raxStop(&ri);
    } else if (!strcasecmp(sub,"whoami") && c->argc == 2) {
        if (c->user != NULL) {
            addReplyBulkCBuffer(c,c->user->name,sdslen(c->user->name));
        } else {
            addReplyNull(c);
        }
    } else if (server.acl_filename[0] == '\0' &&
               (!strcasecmp(sub,"load") || !strcasecmp(sub,"save")))
    {
        addReplyError(c,"This Redis instance is not configured to use an ACL file. You may want to specify users via the ACL SETUSER command and then issue a CONFIG REWRITE (assuming you have a Redis configuration file set) in order to store users in the Redis configuration.");
        return;
    } else if (!strcasecmp(sub,"load") && c->argc == 2) {
        sds errors = ACLLoadFromFile(server.acl_filename);
        if (errors == NULL) {
            addReply(c,shared.ok);
        } else {
            addReplyError(c,errors);
            sdsfree(errors);
        }
    } else if (!strcasecmp(sub,"save") && c->argc == 2) {
        if (ACLSaveToFile(server.acl_filename) == C_OK) {
            addReply(c,shared.ok);
        } else {
            addReplyError(c,"There was an error trying to save the ACLs. "
                            "Please check the server logs for more "
                            "information");
        }
    } else if (!strcasecmp(sub,"cat") && c->argc == 2) {
        void *dl = addReplyDeferredLen(c);
        int j;
        for (j = 0; ACLCommandCategories[j].flag != 0; j++)
            addReplyBulkCString(c,ACLCommandCategories[j].name);
        setDeferredArrayLen(c,dl,j);
    } else if (!strcasecmp(sub,"cat") && c->argc == 3) {
        uint64_t cflag = ACLGetCommandCategoryFlagByName(c->argv[2]->ptr);
        if (cflag == 0) {
            addReplyErrorFormat(c, "Unknown category '%s'", (char*)c->argv[2]->ptr);
            return;
        }
        int arraylen = 0;
        void *dl = addReplyDeferredLen(c);
        dictIterator *di = dictGetIterator(server.orig_commands);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            struct redisCommand *cmd = dictGetVal(de);
            if (cmd->flags & CMD_MODULE) continue;
            if (cmd->flags & cflag) {
                addReplyBulkCString(c,cmd->name);
                arraylen++;
            }
        }
        dictReleaseIterator(di);
        setDeferredArrayLen(c,dl,arraylen);
    } else if (!strcasecmp(sub,"genpass") && (c->argc == 2 || c->argc == 3)) {
        #define GENPASS_MAX_BITS 4096
        char pass[GENPASS_MAX_BITS/8*2]; /* Hex representation. */
        long bits = 256; /* By default generate 256 bits passwords. */

        if (c->argc == 3 && getLongFromObjectOrReply(c,c->argv[2],&bits,NULL)
            != C_OK) return;

        if (bits <= 0 || bits > GENPASS_MAX_BITS) {
            addReplyErrorFormat(c,
                "ACL GENPASS argument must be the number of "
                "bits for the output password, a positive number "
                "up to %d",GENPASS_MAX_BITS);
            return;
        }

        long chars = (bits+3)/4; /* Round to number of characters to emit. */
        getRandomHexChars(pass,chars);
        addReplyBulkCBuffer(c,pass,chars);
    } else if (!strcasecmp(sub,"log") && (c->argc == 2 || c->argc ==3)) {
        long count = 10; /* Number of entries to emit by default. */

        /* Parse the only argument that LOG may have: it could be either
         * the number of entries the user wants to display, or alternatively
         * the "RESET" command in order to flush the old entries. */
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"reset")) {
                listSetFreeMethod(ACLLog,ACLFreeLogEntry);
                listEmpty(ACLLog);
                listSetFreeMethod(ACLLog,NULL);
                addReply(c,shared.ok);
                return;
            } else if (getLongFromObjectOrReply(c,c->argv[2],&count,NULL)
                       != C_OK)
            {
                return;
            }
            if (count < 0) count = 0;
        }

        /* Fix the count according to the number of entries we got. */
        if ((size_t)count > listLength(ACLLog))
            count = listLength(ACLLog);

        addReplyArrayLen(c,count);
        listIter li;
        listNode *ln;
        listRewind(ACLLog,&li);
        mstime_t now = mstime();
        while (count-- && (ln = listNext(&li)) != NULL) {
            ACLLogEntry *le = listNodeValue(ln);
            addReplyMapLen(c,7);
            addReplyBulkCString(c,"count");
            addReplyLongLong(c,le->count);

            addReplyBulkCString(c,"reason");
            char *reasonstr;
            switch(le->reason) {
            case ACL_DENIED_CMD: reasonstr="command"; break;
            case ACL_DENIED_KEY: reasonstr="key"; break;
            case ACL_DENIED_CHANNEL: reasonstr="channel"; break;
            case ACL_DENIED_AUTH: reasonstr="auth"; break;
            default: reasonstr="unknown";
            }
            addReplyBulkCString(c,reasonstr);

            addReplyBulkCString(c,"context");
            char *ctxstr;
            switch(le->context) {
            case ACL_LOG_CTX_TOPLEVEL: ctxstr="toplevel"; break;
            case ACL_LOG_CTX_MULTI: ctxstr="multi"; break;
            case ACL_LOG_CTX_LUA: ctxstr="lua"; break;
            case ACL_LOG_CTX_MODULE: ctxstr="module"; break;
            default: ctxstr="unknown";
            }
            addReplyBulkCString(c,ctxstr);

            addReplyBulkCString(c,"object");
            addReplyBulkCBuffer(c,le->object,sdslen(le->object));
            addReplyBulkCString(c,"username");
            addReplyBulkCBuffer(c,le->username,sdslen(le->username));
            addReplyBulkCString(c,"age-seconds");
            double age = (double)(now - le->ctime)/1000;
            addReplyDouble(c,age);
            addReplyBulkCString(c,"client-info");
            addReplyBulkCBuffer(c,le->cinfo,sdslen(le->cinfo));
        }
    } else if (c->argc == 2 && !strcasecmp(sub,"help")) {
        const char *help[] = {
"CAT [<category>]",
"    List all commands that belong to <category>, or all command categories",
"    when no category is specified.",
"DELUSER <username> [<username> ...]",
"    Delete a list of users.",
"GETUSER <username>",
"    Get the user's details.",
"GENPASS [<bits>]",
"    Generate a secure 256-bit user password. The optional `bits` argument can",
"    be used to specify a different size.",
"LIST",
"    Show users details in config file format.",
"LOAD",
"    Reload users from the ACL file.",
"LOG [<count> | RESET]",
"    Show the ACL log entries.",
"SAVE",
"    Save the current config to the ACL file.",
"SETUSER <username> <attribute> [<attribute> ...]",
"    Create or modify a user with the specified attributes.",
"USERS",
"    List all the registered usernames.",
"WHOAMI",
"    Return the current connection username.",
NULL
        };
        addReplyHelp(c,help);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

void addReplyCommandCategories(client *c, struct redisCommand *cmd) {
    int flagcount = 0;
    void *flaglen = addReplyDeferredLen(c);
    for (int j = 0; ACLCommandCategories[j].flag != 0; j++) {
        if (cmd->flags & ACLCommandCategories[j].flag) {
            addReplyStatusFormat(c, "@%s", ACLCommandCategories[j].name);
            flagcount++;
        }
    }
    setDeferredSetLen(c, flaglen, flagcount);
}

/* AUTH <password>
 * AUTH <username> <password> (Redis >= 6.0 form)
 *
 * When the user is omitted it means that we are trying to authenticate
 * against the default user. */
void authCommand(client *c) {
    /* Only two or three argument forms are allowed. */
    if (c->argc > 3) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }
    /* Always redact the second argument */
    redactClientCommandArgument(c, 1);

    /* Handle the two different forms here. The form with two arguments
     * will just use "default" as username. */
    robj *username, *password;
    if (c->argc == 2) {
        /* Mimic the old behavior of giving an error for the two argument
         * form if no password is configured. */
        if (DefaultUser->flags & USER_FLAG_NOPASS) {
            addReplyError(c,"AUTH <password> called without any password "
                            "configured for the default user. Are you sure "
                            "your configuration is correct?");
            return;
        }

        username = shared.default_username; 
        password = c->argv[1];
    } else {
        username = c->argv[1];
        password = c->argv[2];
        redactClientCommandArgument(c, 2);
    }

    if (ACLAuthenticateUser(c,username,password) == C_OK) {
        addReply(c,shared.ok);
    } else {
        addReplyError(c,"-WRONGPASS invalid username-password pair or user is disabled.");
    }
}

/* Set the password for the "default" ACL user. This implements supports for
 * requirepass config, so passing in NULL will set the user to be nopass. */
void ACLUpdateDefaultUserPassword(sds password) {
    ACLSetUser(DefaultUser,"resetpass",-1);
    if (password) {
        sds aclop = sdscatlen(sdsnew(">"), password, sdslen(password));
        ACLSetUser(DefaultUser,aclop,sdslen(aclop));
        sdsfree(aclop);
    } else {
        ACLSetUser(DefaultUser,"nopass",-1);
    }
}
