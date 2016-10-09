#include <netinet/in.h> // IPPROTO_UDP, IPPROTO_SCTP
#include <anyrtc.h>
#include "utils.h"
#include "ice_gatherer.h"
#include "ice_candidate.h"

#define DEBUG_MODULE "ice-gatherer"
#define DEBUG_LEVEL 7
#include <re_dbg.h>

static void anyrtc_ice_gather_options_destroy(void* arg) {
    struct anyrtc_ice_gather_options* options = arg;

    // Dereference
    list_flush(&options->ice_servers);
}

/*
 * Create a new ICE gather options.
 */
enum anyrtc_code anyrtc_ice_gather_options_create(
        struct anyrtc_ice_gather_options** const optionsp, // de-referenced
        enum anyrtc_ice_gather_policy const gather_policy
) {
    struct anyrtc_ice_gather_options* options;

    // Check arguments
    if (!optionsp) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Allocate
    options = mem_alloc(sizeof(struct anyrtc_ice_gather_options),
                        anyrtc_ice_gather_options_destroy);
    if (!options) {
        return ANYRTC_CODE_NO_MEMORY;
    }

    // Set fields/reference
    options->gather_policy = gather_policy;
    list_init(&options->ice_servers);

    // Set pointer and return
    *optionsp = options;
    return ANYRTC_CODE_SUCCESS;
}

/*
 * Destructor for URLs of the ICE gatherer.
 */
static void anyrtc_ice_server_url_destroy(void* arg) {
    struct anyrtc_ice_server_url* url = arg;

    // Dereference
    mem_deref(url->url);
}

/*
 * Copy a URL for the ICE gatherer.
 */
static enum anyrtc_code anyrtc_ice_server_url_create(
        struct anyrtc_ice_server_url** const urlp, // de-referenced
        char* const url_s // copied
) {
    struct anyrtc_ice_server_url* url;
    enum anyrtc_code error;
    char* copy;

    // Check arguments
    if (!urlp || !url_s) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Allocate
    url = mem_zalloc(sizeof(struct anyrtc_ice_server_url), anyrtc_ice_server_url_destroy);
    if (!url) {
        return ANYRTC_CODE_NO_MEMORY;
    }

    // Copy URL
    error = anyrtc_strdup(&copy, url_s);
    if (error) {
        mem_deref(url);
        return error;
    }

    // Set pointer and return
    url->url = copy;
    *urlp = url;
    return ANYRTC_CODE_SUCCESS;
}

/*
 * Destructor for an existing ICE server.
 */
static void anyrtc_ice_server_destroy(void* arg) {
    struct anyrtc_ice_server* server = arg;

    // Dereference
    list_flush(&server->urls);
    mem_deref(server->username);
    mem_deref(server->credential);
}

/*
 * Add an ICE server to the gather options.
 */
enum anyrtc_code anyrtc_ice_gather_options_add_server(
        struct anyrtc_ice_gather_options* const options,
        char* const * const urls, // copied
        size_t const n_urls,
        char* const username, // nullable, copied
        char* const credential, // nullable, copied
        enum anyrtc_ice_credential_type const credential_type
) {
    struct anyrtc_ice_server* server;
    enum anyrtc_code error = ANYRTC_CODE_SUCCESS;
    size_t i;

    // Check arguments
    if (!options || !urls) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Allocate
    server = mem_zalloc(sizeof(struct anyrtc_ice_server), anyrtc_ice_server_destroy);
    if (!server) {
        return ANYRTC_CODE_NO_MEMORY;
    }

    // Copy URLs to list
    list_init(&server->urls);
    for (i = 0; i < n_urls; ++i) {
        struct anyrtc_ice_server_url* url;

        // Ensure URLs aren't null
        if (!urls[i]) {
            error = ANYRTC_CODE_INVALID_ARGUMENT;
            goto out;
        }

        // Copy URL
        error = anyrtc_ice_server_url_create(&url, urls[i]);
        if (error) {
            goto out;
        }

        // Append URL to list
        list_append(&server->urls, &url->le, url);
    }

    // Set fields
    if (credential_type != ANYRTC_ICE_CREDENTIAL_NONE) {
        if (username) {
            error = anyrtc_strdup(&server->username, username);
            if (error) {
                goto out;
            }
        }
        if (credential) {
            error = anyrtc_strdup(&server->credential, credential);
            if (error) {
                goto out;
            }
        }
    }
    server->credential_type = credential_type; // TODO: Validation needed in case TOKEN is used?

    // Add to options
    list_append(&options->ice_servers, &server->le, server);

out:
    if (error) {
        list_flush(&server->urls);
        mem_deref(server->username);
        mem_deref(server->credential);
        mem_deref(server);
    }
    return error;
}

/*
 * Destructor for an existing ICE gatherer.
 */
static void anyrtc_ice_gatherer_destroy(void* arg) {
    struct anyrtc_ice_gatherer* gatherer = arg;

    // Dereference
    mem_deref(gatherer->ice);
    mem_deref(gatherer->options);
}

/*
 * Create a new ICE gatherer.
 */
enum anyrtc_code anyrtc_ice_gatherer_create(
        struct anyrtc_ice_gatherer** const gathererp, // de-referenced
        struct anyrtc_ice_gather_options* const options, // referenced
        anyrtc_ice_gatherer_state_change_handler* const state_change_handler, // nullable
        anyrtc_ice_gatherer_error_handler* const error_handler, // nullable
        anyrtc_ice_gatherer_local_candidate_handler* const local_candidate_handler, // nullable
        void* const arg // nullable
) {
    struct anyrtc_ice_gatherer* gatherer;
    enum anyrtc_code error;

    // Check arguments
    if (!gathererp || !options) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Allocate
    gatherer = mem_alloc(sizeof(struct anyrtc_ice_gatherer), anyrtc_ice_gatherer_destroy);
    if (!gatherer) {
        return ANYRTC_CODE_NO_MEMORY;
    }

    // Set fields/reference
    gatherer->state = ANYRTC_ICE_GATHERER_NEW;
    gatherer->options = mem_ref(options);
    gatherer->state_change_handler = state_change_handler;
    gatherer->error_handler = error_handler;
    gatherer->local_candidate_handler = local_candidate_handler;
    gatherer->arg = arg;

    // Generate random username fragment and password for ICE
    rand_str(gatherer->ice_username_fragment, sizeof(gatherer->ice_username_fragment));
    rand_str(gatherer->ice_password, sizeof(gatherer->ice_password));

    // Set ICE configuration
    // TODO: Add parameters to function arguments?
    gatherer->ice_config.debug = true;
    gatherer->ice_config.trace = true;
    gatherer->ice_config.ansi = true;
    gatherer->ice_config.enable_prflx = true;
    error = anyrtc_code_re_translate(trice_alloc(
            &gatherer->ice, &gatherer->ice_config, ROLE_UNKNOWN,
            gatherer->ice_username_fragment, gatherer->ice_password));
    if (error) {
        goto out;
    }

out:
    if (error) {
        mem_deref(gatherer->ice);
        mem_deref(gatherer->options);
        mem_deref(gatherer);
    } else {
        // Set pointer
        *gathererp = gatherer;
    }
    return error;
}

/*
 * Close the ICE gatherer.
 */
enum anyrtc_code anyrtc_ice_gatherer_close(
        struct anyrtc_ice_gatherer* const gatherer
) {
    // Check arguments
    if (!gatherer) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }

    // Already closed?
    if (gatherer->state == ANYRTC_ICE_GATHERER_CLOSED) {
        return ANYRTC_CODE_SUCCESS;
    }

    // Stop ICE checklist (if running)
    trice_checklist_stop(gatherer->ice);

    // Remove ICE agent
    gatherer->ice = mem_deref(gatherer->ice);

    // Set state to closed and return
    gatherer->state = ANYRTC_ICE_GATHERER_CLOSED;
    return ANYRTC_CODE_SUCCESS;
}

/*
 * Local interfaces callback.
  */
static bool interface_handler(
        char const* const interface,
        struct sa const* const address,
        void* const arg
) {
    struct anyrtc_ice_gatherer* const gatherer = arg;
    struct ice_lcand* candidate;
    uint32_t priority;
    int error;

    // Ignore loopback and linklocal addresses
    if (sa_is_linklocal(address) || sa_is_loopback(address)) {
        return false; // Continue gathering
    }

    DEBUG_PRINTF("Gathered local interface %j\n", address);

    // Add UDP candidate to ICE gatherer (if not already added)
    // TODO: Check if already added?
    // TODO: Set component id properly
    priority = anyrtc_ice_candidate_calculate_priority(
            ICE_CAND_TYPE_HOST, IPPROTO_UDP, ICE_TCP_ACTIVE);
    error = trice_lcand_add(
            &candidate, gatherer->ice, 1, IPPROTO_UDP, priority, address,
            NULL, ICE_CAND_TYPE_HOST, NULL, ICE_TCP_ACTIVE, NULL, ANYRTC_LAYER_ICE);
    if (error) {
        DEBUG_WARNING("Could not add UDP candidate: %m", error);
        return false; // Continue gathering
    }

    // TODO: Add TCP candidate to ICE gatherer (if not already added) (?)

    // TODO: Gather srflx candidates
    DEBUG_PRINTF("TODO: Gather srflx candidates for %j\n", address);
    // TODO: Gather relay candidates
    DEBUG_PRINTF("TODO: Gather relay candidates for %j\n", address);

    // Continue gathering
    return false;
}

/*
 * Start gathering using an ICE gatherer.
 */
enum anyrtc_code anyrtc_ice_gatherer_gather(
        struct anyrtc_ice_gatherer* const gatherer,
        struct anyrtc_ice_gather_options* options // referenced, nullable
) {
    enum anyrtc_code error;

    // Check arguments
    if (!gatherer) {
        return ANYRTC_CODE_INVALID_ARGUMENT;
    }
    if (!options) {
        options = gatherer->options;
    }

    // Check state
    if (gatherer->state == ANYRTC_ICE_GATHERER_CLOSED) {
        return ANYRTC_CODE_INVALID_STATE;
    }

    // Already gathering?
    if (gatherer->state == ANYRTC_ICE_GATHERER_GATHERING) {
        return ANYRTC_CODE_SUCCESS;
    }

    // Update state
    gatherer->state = ANYRTC_ICE_GATHERER_GATHERING;

    // Start gathering host candidates
    if (options->gather_policy != ANYRTC_ICE_GATHER_NOHOST) {
        net_if_apply(interface_handler, gatherer);
    }

    // TODO: Debug only
    error = anyrtc_code_re_translate(trice_debug(&anyrtc_stdout, gatherer->ice));
    return error;
}
