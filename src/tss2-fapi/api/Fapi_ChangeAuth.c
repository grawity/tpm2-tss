/* SPDX-License-Identifier: BSD-2-Clause */
/*******************************************************************************
 * Copyright 2018-2019, Fraunhofer SIT sponsored by Infineon Technologies AG
 * All rights reserved.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "tss2_fapi.h"
#include "fapi_int.h"
#include "fapi_util.h"
#include "tss2_esys.h"
#define LOGMODULE fapi
#include "util/log.h"
#include "util/aux_util.h"
#include "fapi_crypto.h"

/** One-Call function for Fapi_ChangeAuth
 *
 * Changes the Authorization data of an entity found at keyPath. The parameter
 * authValue is a 0-terminated UTF-8 encoded password.
 * If it is longer than the digest size of the entity's nameAlg, it will be
 * hashed according the the TPM specification part 1, rev 138, section 19.6.4.3.
 *
 * @param [in, out] context The FAPI_CONTEXT
 * @param [in] entityPath The path to the entity to modify
 * @param [in] authValue The new 0-terminated password to set for the entity.
 *             May be NULL
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context or entityPath is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_PATH: if entityPath does not map to a FAPI entity.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_STORAGE_ERROR: If the entity with the new password
 *         cannot be saved.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 */
TSS2_RC
Fapi_ChangeAuth(
    FAPI_CONTEXT *context,
    char   const *entityPath,
    char   const *authValue)
{
    LOG_TRACE("called for context:%p", context);

    TSS2_RC r, r2;

    /* Check for NULL parameters */
    check_not_null(context);
    check_not_null(entityPath);

    /* Check whether TCTI and ESYS are initialized */
    return_if_null(context->esys, "Command can't be executed in none TPM mode.",
                   TSS2_FAPI_RC_NO_TPM);

    /* If the async state automata of FAPI shall be tested, then we must not set
       the timeouts of ESYS to blocking mode.
       During testing, the mssim tcti will ensure multiple re-invocations.
       Usually however the synchronous invocations of FAPI shall instruct ESYS
       to block until a result is available. */
#ifndef TEST_FAPI_ASYNC
    r = Esys_SetTimeout(context->esys, TSS2_TCTI_TIMEOUT_BLOCK);
    return_if_error_reset_state(r, "Set Timeout to blocking");
#endif /* TEST_FAPI_ASYNC */

    r = Fapi_ChangeAuth_Async(context, entityPath, authValue);
    return_if_error_reset_state(r, "Entity_ChangeAuth");

    do {
        /* We wait for file I/O to be ready if the FAPI state automata
           are in a file I/O state. */
        r = ifapi_io_poll(&context->io);
        return_if_error(r, "Something went wrong with IO polling");

        /* Repeatedly call the finish function, until FAPI has transitioned
           through all execution stages / states of this invocation. */
        r = Fapi_ChangeAuth_Finish(context);
    } while ((r & ~TSS2_RC_LAYER_MASK) == TSS2_BASE_RC_TRY_AGAIN);

    /* Reset the ESYS timeout to non-blocking, immediate response. */
    r2 = Esys_SetTimeout(context->esys, 0);
    return_if_error(r2, "Set Timeout to non-blocking");

    return_if_error_reset_state(r, "Entity_ChangeAuth");

    LOG_TRACE("finsihed");
    return TSS2_RC_SUCCESS;
}

/** Asynchronous function for Fapi_ChangeAuth
 *
 * Changes the Authorization data of an entity found at keyPath. The parameter
 * authValue is a 0-terminated UTF-8 encoded password.
 * If it is longer than the digest size of the entity's nameAlg, it will be
 * hashed according the the TPM specification part 1, rev 138, section 19.6.4.3.
 *
 * Call Fapi_ChangeAuth_Finish to finish the execution of this command.

 * @param [in, out] context The FAPI_CONTEXT
 * @param [in] entityPath The path to the entity to modify
 * @param [in] authValue The new 0-terminated password to set for the entity.
 *         May be NULL
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context or entityPath is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_PATH: if entityPath does not map to a FAPI entity.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_STORAGE_ERROR: If the entity with the new password
 *         cannot be saved.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 */
TSS2_RC
Fapi_ChangeAuth_Async(
    FAPI_CONTEXT  *context,
    char    const *entityPath,
    char    const *authValue)
{
    LOG_TRACE("called for context:%p", context);
    LOG_TRACE("entityPath: %s", entityPath);
    LOG_TRACE("authValue: %s", authValue);

    TSS2_RC r;

    /* Check for NULL parameters */
    check_not_null(context);
    check_not_null(entityPath);

    /* Helpful pointers */
    IFAPI_Entity_ChangeAuth * command = &(context->cmd.Entity_ChangeAuth);

    r = ifapi_session_init(context);
    return_if_error(r, "Initialize Entity_ChangeAuth");

    context->loadKey.parent_handle = ESYS_TR_NONE;
    command->handle = ESYS_TR_NONE;
    memset(&command->object, 0, sizeof(IFAPI_OBJECT));
    strdup_check(command->entityPath, entityPath, r, error_cleanup);
    if(authValue != NULL) {
        strdup_check(command->authValue, authValue, r, error_cleanup);
    } else {
        strdup_check(command->authValue, "", r, error_cleanup);
    }
    command->handle = ESYS_TR_NONE;

    r = ifapi_get_sessions_async(context,
                                 IFAPI_SESSION_GENEK | IFAPI_SESSION1,
                                 TPMA_SESSION_DECRYPT, 0);
    goto_if_error_reset_state(r, "Create sessions", error_cleanup);

    if (strlen(command->authValue) > sizeof(TPMU_HA)) {
        LOG_ERROR("authValue to big. (Should be <= %zu", sizeof(TPMU_HA));
        r = TSS2_FAPI_RC_BAD_VALUE;
        goto error_cleanup;
    }

    /* Copy new auth value to appropriate structure in context */
    if (command->authValue) {
        command->newAuthValue.size =
            strlen(command->authValue);
        memcpy(&command->newAuthValue.buffer[0],
               command->authValue,
               command->newAuthValue.size);
    } else {
        command->newAuthValue.size = 0;
    }

    context->state = ENTITY_CHANGE_AUTH_WAIT_FOR_SESSION;
    LOG_TRACE("finsihed");
    return TSS2_RC_SUCCESS;

error_cleanup:
    SAFE_FREE(command->entityPath);
    SAFE_FREE(command->authValue);
    return r;
}

/** Asynchronous finish function for Fapi_ChangeAuth
 *
 * This function should be called after a previous Fapi_ChangeAuth_Async.
 *
 * @param [in, out] context The FAPI_CONTEXT
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 * @retval TSS2_FAPI_RC_TRY_AGAIN: if the asynchronous operation is not yet
 *         complete. Call this function again later.
 */
TSS2_RC
Fapi_ChangeAuth_Finish(
    FAPI_CONTEXT *context)
{
    LOG_TRACE("called for context:%p", context);

    TSS2_RC r;
    ESYS_TR auth_session;

    /* Check for NULL parameters */
    check_not_null(context);

    /* Helpful pointers */
    IFAPI_Entity_ChangeAuth * command = &(context->cmd.Entity_ChangeAuth);
    IFAPI_OBJECT * object = &command->object;
    const IFAPI_PROFILE *profile;

    switch (context->state) {
        statecase(context->state, ENTITY_CHANGE_AUTH_WAIT_FOR_SESSION)
            r = ifapi_profiles_get(&context->profiles, command->entityPath,
                    &profile);
            goto_if_error_reset_state(r, " FAPI create session", error_cleanup);

            r = ifapi_get_sessions_finish(context, profile);
            return_try_again(r);

            goto_if_error_reset_state(r, " FAPI create session", error_cleanup);

            if (ifapi_path_type_p(command->entityPath,
                    IFAPI_NV_PATH)) {
                r = ifapi_keystore_load_async(&context->keystore, &context->io,
                        command->entityPath);
                return_if_error_reset_state(r, "Could not open: %s",
                        command->entityPath);

                context->state = ENTITY_CHANGE_AUTH_WAIT_FOR_NV_READ;
                return TSS2_FAPI_RC_TRY_AGAIN;
            }

            command->hierarchy_handle =
                ifapi_get_hierary_handle(command->entityPath);

            if (command->hierarchy_handle) {
                context->state = ENTITY_CHANGE_AUTH_HIERARCHY_READ;
                r = ifapi_keystore_load_async(&context->keystore, &context->io,
                        command->entityPath);
                return_if_error_reset_state(r, "Could not open: %s",
                        command->entityPath);

                return TSS2_FAPI_RC_TRY_AGAIN;
            }

            r = ifapi_load_keys_async(context, command->entityPath);
            goto_if_error(r, "Load keys.", error_cleanup);

            context->state = ENTITY_CHANGE_AUTH_WAIT_FOR_KEY;
            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_WAIT_FOR_KEY)
            r = ifapi_load_keys_finish(context, IFAPI_NOT_FLUSH_PARENT,
                    &command->handle,
                    &command->key_object);
            return_try_again(r);

            context->state = ENTITY_CHANGE_AUTH_WAIT_FOR_KEY_AUTH;
            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_WAIT_FOR_KEY_AUTH)
            /* Authorize the object with the old authorization */
            object = command->key_object;
            r = ifapi_authorize_object(context, object, &auth_session);

            return_try_again(r);
            goto_if_error_reset_state(r, "Authorize key.", error_cleanup);

            r = Esys_ObjectChangeAuth_Async(context->esys,
                    command->handle,
                    context->loadKey.parent_handle,
                    auth_session,
                    ESYS_TR_NONE, ESYS_TR_NONE,
                    &command->newAuthValue);
            goto_if_error(r, "Error: Sign", error_cleanup);

            context->state = ENTITY_CHANGE_AUTH_AUTH_SENT;
            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_AUTH_SENT)
            r = Esys_ObjectChangeAuth_Finish(context->esys,
                    &command->newPrivate);
            return_try_again(r);

            goto_if_error(r, "Error: Entity ChangeAuth", error_cleanup);

            object = command->key_object;
            object->misc.key.private.size = command->newPrivate->size;

            /* Old private buffer will be overwritten*/
            free(object->misc.key.private.buffer);
            object->misc.key.private.buffer = malloc(object->misc.key.private.size);
            goto_if_null2(object->misc.key.private.buffer, "Out of memory.",
                    r, TSS2_FAPI_RC_MEMORY, error_cleanup);

            memcpy(object->misc.key.private.buffer,
                    &command->newPrivate->buffer[0],
                    object->misc.key.private.size);
            free(command->newPrivate);
            r = Esys_FlushContext_Async(context->esys,
                    command->handle);
            goto_if_error(r, "Error: FlushContext", error_cleanup);

            command->handle = ESYS_TR_NONE;
            context->state = ENTITY_CHANGE_AUTH_WAIT_FOR_FLUSH;
            return TSS2_FAPI_RC_TRY_AGAIN;

        statecase(context->state, ENTITY_CHANGE_AUTH_WAIT_FOR_FLUSH)
            r = Esys_FlushContext_Finish(context->esys);
            return_try_again(r);

            goto_if_error(r, "Error: ObjectChangeAuth", error_cleanup);

            if (! context->loadKey.parent_handle_persistent
                    && context->loadKey.parent_handle != ESYS_TR_NONE) {
                r = Esys_FlushContext_Async(context->esys, context->loadKey.parent_handle);
                goto_if_error(r, "Flush parent", error_cleanup);

                context->loadKey.parent_handle = ESYS_TR_NONE;
                return TSS2_FAPI_RC_TRY_AGAIN;
            }

            /* Serialize key with new private data */
            object = command->key_object;

            if (strlen(command->authValue) > 0)
                object->misc.key.with_auth = TPM2_YES;
            else
                object->misc.key.with_auth = TPM2_NO;
            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_WRITE_PREPARE)
            /* Perform esys serialization if necessary */
            r = ifapi_esys_serialize_object(context->esys, object);
            goto_if_error(r, "Prepare serialization", error_cleanup);

            /* Start writing the NV object to the key store */
            r = ifapi_keystore_store_async(&context->keystore, &context->io,
                    command->entityPath,
                    object);
            goto_if_error_reset_state(r, "Could not open: %sh", error_cleanup,
                    command->entityPath);
            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_WRITE)
            /* Finish writing the object to the key store */
            r = ifapi_keystore_store_finish(&context->keystore, &context->io);
            return_try_again(r);
            return_if_error_reset_state(r, "write_finish failed");

            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_CLEANUP)
            r = ifapi_cleanup_session(context);
            try_again_or_error_goto(r, "Cleanup", error_cleanup);

            context->state = _FAPI_STATE_INIT;
            LOG_TRACE("success");
            r = TSS2_RC_SUCCESS;
            break;

        statecase(context->state, ENTITY_CHANGE_AUTH_WAIT_FOR_NV_READ)
            /* Get object from file */
            r = ifapi_keystore_load_finish(&context->keystore, &context->io,
                    &command->object);
            return_try_again(r);
            return_if_error_reset_state(r, "read_finish failed");

            r = ifapi_initialize_object(context->esys, &command->object);
            goto_if_error_reset_state(r, "Initialize NV object", error_cleanup);

            context->state = ENTITY_CHANGE_AUTH_WAIT_FOR_NV_AUTH;
            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_WAIT_FOR_NV_AUTH)
            /* Authorize the object with with the policies
               auth value and command code */
            r = ifapi_authorize_object(context, object, &auth_session);
            return_try_again(r);
            goto_if_error(r, "Authorize NV object.", error_cleanup);

            r = Esys_NV_ChangeAuth_Async(context->esys,
                    context->nv_cmd.nv_object.handle,
                    auth_session,
                    ESYS_TR_NONE,
                    ESYS_TR_NONE,
                    &command->newAuthValue);
            goto_if_error(r, "Error: NV_ChangeAuth", error_cleanup);

            context->state = ENTITY_CHANGE_AUTH_WAIT_FOR_NV_CHANGE_AUTH;
            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_WAIT_FOR_NV_CHANGE_AUTH)
            r = Esys_NV_ChangeAuth_Finish(context->esys);
            return_try_again(r);

            goto_if_error(r, "Error: Entity ChangeAuth", error_cleanup);

            if (strlen(command->authValue) > 0)
                object->misc.nv.with_auth = TPM2_YES;
            else
                object->misc.nv.with_auth = TPM2_NO;

            context->state =  ENTITY_CHANGE_AUTH_WRITE_PREPARE;
            return TSS2_FAPI_RC_TRY_AGAIN;

            statecase(context->state, ENTITY_CHANGE_AUTH_HIERARCHY_READ)
                r = ifapi_keystore_load_finish(&context->keystore, &context->io, object);
            return_try_again(r);
            return_if_error_reset_state(r, "read_finish failed");

            r = ifapi_initialize_object(context->esys, &command->object);
            goto_if_error_reset_state(r, "Initialize NV object", error_cleanup);

            command->object.handle
                = command->hierarchy_handle;

            context->state = ENTITY_CHANGE_AUTH_HIERARCHY_AUTHORIZE;
            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_HIERARCHY_AUTHORIZE)
            r = ifapi_authorize_object(context, &command->object, &auth_session);
            return_try_again(r);
            goto_if_error(r, "Authorize hierarchy.", error_cleanup);

            context->state = ENTITY_CHANGE_AUTH_HIERARCHY_CHANGE_AUTH;
            fallthrough;

        statecase(context->state, ENTITY_CHANGE_AUTH_HIERARCHY_CHANGE_AUTH)
            r = ifapi_change_auth_hierarchy(context,
                    command->hierarchy_handle,
                    &command->object,
                    &command->newAuthValue);
            return_try_again(r);
            goto_if_error(r, "Change auth hierarchy.", error_cleanup);

            context->state = ENTITY_CHANGE_AUTH_WRITE_PREPARE;
            return TSS2_FAPI_RC_TRY_AGAIN;

        statecasedefault(context->state);
    }

error_cleanup:
    /* In error cases object might not be flushed. */
    if (context->loadKey.parent_handle != ESYS_TR_NONE)
        Esys_FlushContext(context->esys, context->loadKey.parent_handle);
    if (command->handle != ESYS_TR_NONE)
        Esys_FlushContext(context->esys, command->handle);
    ifapi_session_clean(context);
    ifapi_cleanup_ifapi_object(object);
    ifapi_cleanup_ifapi_object(context->loadKey.key_object);
    ifapi_cleanup_ifapi_object(&context->loadKey.auth_object);
    ifapi_cleanup_ifapi_object(&context->createPrimary.pkey_object);
    ifapi_cleanup_ifapi_object(command->key_object);
    SAFE_FREE(command->entityPath);
    SAFE_FREE(command->authValue);
    LOG_TRACE("finsihed");
    return r;
}
