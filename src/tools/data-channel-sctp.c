#include <stdio.h>
#include <anyrtc.h>

/* TODO: Replace with zf_log */
#define DEBUG_MODULE "data-channel-sctp-app"
#define DEBUG_LEVEL 7
#include <re_dbg.h>

#define EOE(code) exit_on_error(code, __FILE__, __LINE__)

struct client;

struct client {
    char* name;
    struct anyrtc_ice_gather_options* gather_options;
    struct anyrtc_data_channel_parameters* channel_parameters;
    struct anyrtc_ice_parameters* ice_parameters;
    struct anyrtc_dtls_parameters* dtls_parameters;
    struct anyrtc_sctp_capabilities* sctp_capabilities;
    enum anyrtc_ice_role const role;
    struct anyrtc_certificate* certificate;
    uint16_t sctp_port;
    struct anyrtc_ice_gatherer* gatherer;
    struct anyrtc_ice_transport* ice_transport;
    struct anyrtc_dtls_transport* dtls_transport;
    struct anyrtc_sctp_transport* sctp_transport;
    struct anyrtc_data_transport* data_transport;
    struct anyrtc_data_channel* data_channel;
    struct client* other_client;
};

static void before_exit() {
    // Close
    anyrtc_close();

    // Check memory leaks
    tmr_debug();
    mem_debug();
}

static void exit_on_error(enum anyrtc_code code, char const* const file, uint32_t line) {
    switch (code) {
        case ANYRTC_CODE_SUCCESS:
            return;
        case ANYRTC_CODE_NOT_IMPLEMENTED:
            fprintf(stderr, "Not implemented in %s %"PRIu32"\n",
                    file, line);
            return;
        default:
            fprintf(stderr, "Error in %s %"PRIu32" (%d): %s\n",
                    file, line, code, anyrtc_code_to_str(code));
            before_exit();
            exit((int) code);
    }
}

static void ice_gatherer_state_change_handler(
        enum anyrtc_ice_gatherer_state const state, // read-only
        void* const arg
) {
    struct client* const client = arg;
    char const * const state_name = anyrtc_ice_gatherer_state_to_name(state);
    (void) arg;
    DEBUG_PRINTF("(%s) ICE gatherer state: %s\n", client->name, state_name);
}

static void ice_gatherer_error_handler(
        struct anyrtc_ice_candidate* const host_candidate, // read-only, nullable
        char const * const url, // read-only
        uint16_t const error_code, // read-only
        char const * const error_text, // read-only
        void* const arg
) {
    struct client* const client = arg;
    (void) host_candidate; (void) error_code; (void) arg;
    DEBUG_PRINTF("(%s) ICE gatherer error, URL: %s, reason: %s\n", client->name, url, error_text);
}

static void ice_gatherer_local_candidate_handler(
        struct anyrtc_ice_candidate* const candidate,
        char const * const url, // read-only
        void* const arg
) {
    struct client* const client = arg;
    (void) candidate; (void) arg;

    if (candidate) {
        DEBUG_PRINTF("(%s) ICE gatherer local candidate, URL: %s\n", client->name, url);
    } else {
        DEBUG_PRINTF("(%s) ICE gatherer last local candidate\n", client->name);
    }

    // Add to other client as remote candidate
    EOE(anyrtc_ice_transport_add_remote_candidate(client->other_client->ice_transport, candidate));
}

static void ice_transport_state_change_handler(
        enum anyrtc_ice_transport_state const state,
        void* const arg
) {
    struct client* const client = arg;
    char const * const state_name = anyrtc_ice_transport_state_to_name(state);
    (void) arg;
    DEBUG_PRINTF("(%s) ICE transport state: %s\n", client->name, state_name);
}

static void ice_transport_candidate_pair_change_handler(
        struct anyrtc_ice_candidate* const local, // read-only
        struct anyrtc_ice_candidate* const remote, // read-only
        void* const arg
) {
    struct client* const client = arg;
    (void) local; (void) remote;
    DEBUG_PRINTF("(%s) ICE transport candidate pair change\n", client->name);
}

static void dtls_transport_state_change_handler(
        enum anyrtc_dtls_transport_state const state, // read-only
        void* const arg
) {
    struct client* const client = arg;
    char const * const state_name = anyrtc_dtls_transport_state_to_name(state);
    DEBUG_PRINTF("(%s) DTLS transport state change: %s\n", client->name, state_name);
}

static void dtls_transport_error_handler(
    /* TODO: error.message (probably from OpenSSL) */
    void* const arg
) {
    struct client* const client = arg;
    // TODO: Print error message
    DEBUG_PRINTF("(%s) DTLS transport error: %s\n", client->name, "???");
}

static void sctp_transport_state_change_handler(
    enum anyrtc_sctp_transport_state const state,
    void* const arg
) {
    struct client* const client = arg;
    char const * const state_name = anyrtc_sctp_transport_state_to_name(state);
    DEBUG_PRINTF("(%s) SCTP transport state change: %s\n", client->name, state_name);
}

static void data_channel_handler(
        struct anyrtc_data_channel* const data_channel, // read-only, MUST be referenced when used
        void* const arg
) {
    struct client* const client = arg;
    DEBUG_PRINTF("(%s) New data channel instance\n", client->name);
}

static void data_channel_open_handler(
        void* const arg
) {
    struct client* const client = arg;
    DEBUG_PRINTF("(%s) Data channel open: %s\n", client->name, client->channel_parameters->label);
}

static void data_channel_buffered_amount_low_handler(
        void* const arg
) {
    struct client* const client = arg;
    DEBUG_PRINTF("(%s) Data channel buffered amount low: %s\n", client->name,
                 client->channel_parameters->label);
}

static void data_channel_error_handler(
        void* const arg
) {
    struct client* const client = arg;
    DEBUG_PRINTF("(%s) Data channel error: %s\n", client->name, client->channel_parameters->label);
}

static void data_channel_close_handler(
        void* const arg
) {
    struct client* const client = arg;
    DEBUG_PRINTF("(%s) Data channel closed: %s\n", client->name, client->channel_parameters->label);
}

static void data_channel_message_handler(
        uint8_t const * const data, // read-only
        uint32_t const size,
        void* const arg
) {
    struct client* const client = arg;
    DEBUG_PRINTF("(%s) Incoming message for data channel %s: %"PRIu32" bytes\n",
                 client->name, client->channel_parameters->label, size);
}

static void signal_handler(
        int sig
) {
    DEBUG_INFO("Got signal: %d, terminating...\n", sig);
    re_cancel();
}

static void client_init(
        struct client* const local
) {
    // Generate certificates
    EOE(anyrtc_certificate_generate(&local->certificate, NULL));
    struct anyrtc_certificate* certificates[] = {local->certificate};

    // Create ICE gatherer
    EOE(anyrtc_ice_gatherer_create(
            &local->gatherer, local->gather_options,
            ice_gatherer_state_change_handler, ice_gatherer_error_handler,
            ice_gatherer_local_candidate_handler, local));

    // Create ICE transport
    EOE(anyrtc_ice_transport_create(
            &local->ice_transport, local->gatherer,
            ice_transport_state_change_handler, ice_transport_candidate_pair_change_handler,
            local));

    // Create DTLS transport
    EOE(anyrtc_dtls_transport_create(
            &local->dtls_transport, local->ice_transport, certificates,
            sizeof(certificates) / sizeof(certificates[0]),
            dtls_transport_state_change_handler, dtls_transport_error_handler, local));

    // Create SCTP transport
    EOE(anyrtc_sctp_transport_create(
            &local->sctp_transport, local->dtls_transport, local->sctp_port,
            data_channel_handler, sctp_transport_state_change_handler, local));

    // Get data transport
    EOE(anyrtc_sctp_transport_get_data_transport(
            &local->data_transport, local->sctp_transport));

    // Create data channel
    EOE(anyrtc_data_channel_create(
            &local->data_channel, local->data_transport, local->channel_parameters,
            data_channel_open_handler, data_channel_buffered_amount_low_handler,
            data_channel_error_handler, data_channel_close_handler, data_channel_message_handler,
            local));
}

static void client_start(
        struct client* const local,
        struct client* const remote
) {
    // Get & set ICE parameters
    EOE(anyrtc_ice_gatherer_get_local_parameters(
            &local->ice_parameters, remote->gatherer));

    // Start gathering
    EOE(anyrtc_ice_gatherer_gather(local->gatherer, NULL));

    // Start ICE transport
    EOE(anyrtc_ice_transport_start(
            local->ice_transport, local->gatherer, local->ice_parameters, local->role));

    // Get DTLS parameters
    EOE(anyrtc_dtls_transport_get_local_parameters(
            &remote->dtls_parameters, remote->dtls_transport));

    // Start DTLS transport
    EOE(anyrtc_dtls_transport_start(
            local->dtls_transport, remote->dtls_parameters));

    // Get SCTP capabilities
    EOE(anyrtc_sctp_transport_get_capabilities(
            &remote->sctp_capabilities, remote->sctp_transport));

    // Start SCTP transport
    EOE(anyrtc_sctp_transport_start(
            local->sctp_transport, remote->sctp_capabilities));
}

static void client_stop(
        struct client* const client
) {
    // Stop transports & close gatherer
    EOE(anyrtc_data_channel_close(client->data_channel));
    EOE(anyrtc_sctp_transport_stop(client->sctp_transport));
    EOE(anyrtc_dtls_transport_stop(client->dtls_transport));
    EOE(anyrtc_ice_transport_stop(client->ice_transport));
    EOE(anyrtc_ice_gatherer_close(client->gatherer));

    // Dereference & close
    client->data_channel = mem_deref(client->data_channel);
    client->sctp_capabilities = mem_deref(client->sctp_capabilities);
    client->dtls_parameters = mem_deref(client->dtls_parameters);
    client->ice_parameters = mem_deref(client->ice_parameters);
    client->data_transport = mem_deref(client->data_transport);
    client->sctp_transport = mem_deref(client->sctp_transport);
    client->dtls_transport = mem_deref(client->dtls_transport);
    client->ice_transport = mem_deref(client->ice_transport);
    client->gatherer = mem_deref(client->gatherer);
    client->certificate = mem_deref(client->certificate);
}

int main(int argc, char* argv[argc + 1]) {
    struct anyrtc_ice_gather_options* gather_options;
    char* const stun_google_com_urls[] = {"stun.l.google.com:19302", "stun1.l.google.com:19302"};
    char* const turn_zwuenf_org_urls[] = {"turn.zwuenf.org"};
    struct anyrtc_data_channel_parameters* channel_parameters;

    // Initialise
    EOE(anyrtc_init());

    // Debug
    // TODO: This should be replaced by our own debugging system
    dbg_init(DBG_DEBUG, DBG_ALL);
    DEBUG_PRINTF("Init\n");

    // Create ICE gather options
    EOE(anyrtc_ice_gather_options_create(&gather_options, ANYRTC_ICE_GATHER_ALL));

    // Add ICE servers to ICE gather options
    EOE(anyrtc_ice_gather_options_add_server(
            gather_options, stun_google_com_urls,
            sizeof(stun_google_com_urls) / sizeof(stun_google_com_urls[0]),
            NULL, NULL, ANYRTC_ICE_CREDENTIAL_NONE));
    EOE(anyrtc_ice_gather_options_add_server(
            gather_options, turn_zwuenf_org_urls,
            sizeof(turn_zwuenf_org_urls) / sizeof(turn_zwuenf_org_urls[0]),
            "bruno", "onurb", ANYRTC_ICE_CREDENTIAL_PASSWORD));

    // Create data channel parameters
    EOE(anyrtc_data_channel_parameters_create(
            &channel_parameters, "cat-noises", ANYRTC_DATA_CHANNEL_TYPE_RELIABLE_ORDERED,
            0, NULL, false, 0));

    // Initialise clients
    struct client a = {
            .name = "A",
            .gather_options = gather_options,
            .channel_parameters = channel_parameters,
            .ice_parameters = NULL,
            .dtls_parameters = NULL,
            .role = ANYRTC_ICE_ROLE_CONTROLLING,
            .certificate = NULL,
            .sctp_port = 6000,
            .gatherer = NULL,
            .ice_transport = NULL,
            .dtls_transport = NULL,
            .sctp_transport = NULL,
            .other_client = NULL,
    };
    struct client b = {
            .name = "B",
            .gather_options = gather_options,
            .channel_parameters = channel_parameters,
            .ice_parameters = NULL,
            .dtls_parameters = NULL,
            .role = ANYRTC_ICE_ROLE_CONTROLLED,
            .certificate = NULL,
            .sctp_port = 5000,
            .gatherer = NULL,
            .ice_transport = NULL,
            .dtls_transport = NULL,
            .sctp_transport = NULL,
            .other_client = NULL,
    };
    a.other_client = &b;
    b.other_client = &a;
    client_init(&a);
    client_init(&b);

    // Start clients
    client_start(&a, &b);
    client_start(&b, &a);

    // Start main loop
    // TODO: Wrap re_main?
    // TODO: Stop main loop once gathering is complete
    EOE(anyrtc_error_to_code(re_main(signal_handler)));

    // Stop clients
    client_stop(&a);
    client_stop(&b);

    // Free
    mem_deref(channel_parameters);
    mem_deref(gather_options);

    // Bye
    before_exit();
    return 0;
}