#include "karu/karu.h"

#include <string.h>

int main(void) {
    if (karu_api_version() != KARU_API_VERSION)
        return 1;
    if (strcmp(karu_status_string(KARU_ERR_CONFIG), "invalid configuration") != 0)
        return 2;

    karu_config* config = NULL;
    karu_client* client = NULL;
    if (karu_config_create_empty(&config) != KARU_OK)
        return 3;
    if (karu_config_set_option(config, "KARU_CONCURRENCY", "4") != KARU_OK)
        return 4;
    if (karu_client_create(config, &client) != KARU_OK)
        return 5;
    if (karu_client_concurrency(client) != 4)
        return 6;
    int match = 0;
    if (karu_client_matches_config(client, config, &match) != KARU_OK || !match)
        return 7;
    if (karu_config_set_option(config, "KARU_CONCURRENCY", "5") != KARU_OK)
        return 8;
    if (karu_client_matches_config(client, config, &match) != KARU_OK || match)
        return 9;

    karu_locator* locator = NULL;
    if (karu_resolve("/vsis3_streaming/bucket/key", &locator) != KARU_ERR_UNSUPPORTED)
        return 10;
    if (karu_resolve("/vsis3/bucket/key", &locator) != KARU_OK)
        return 11;
    if (!karu_locator_is_remote(locator))
        return 12;
    karu_locator_free(locator);
    locator = NULL;
    if (karu_resolve("/vsisource/account/product/key", &locator) != KARU_OK)
        return 13;
    if (strcmp(karu_locator_uri(locator), "/vsisource/account/product/key") != 0)
        return 14;
    karu_locator_free(locator);
    locator = NULL;
    if (karu_resolve("local.bin", &locator) != KARU_OK)
        return 15;
    if (karu_locator_is_remote(locator))
        return 16;
    unsigned char byte = 0;
    karu_req unbounded = {locator, 0, KARU_TO_END, &byte, NULL, NULL};
    karu_batch* batch = NULL;
    if (karu_client_submit(client, &unbounded, 1, &batch) != KARU_ERR_INVALID || batch != NULL)
        return 17;

    karu_submit_options options = KARU_SUBMIT_OPTIONS_INIT;
    options.coalesce_gap = 0;
    if (karu_client_submit_with(client, NULL, 0, &options, &batch) != KARU_OK || batch == NULL)
        return 18;
    karu_done done = {0};
    if (karu_batch_next(batch, &done, 0) != KARU_END)
        return 19;
    karu_batch_free(batch);
    batch = NULL;
    if (karu_client_fetch_with(client, NULL, 0, &options) != KARU_OK)
        return 20;

    options.coalesce_limit = 0;
    if (karu_client_submit_with(client, NULL, 0, &options, &batch) != KARU_ERR_INVALID ||
        batch != NULL)
        return 21;
    options.coalesce_limit = KARU_INHERIT;
    options.coalesce_parts = 0;
    if (karu_client_submit_with(client, NULL, 0, &options, &batch) != KARU_ERR_INVALID ||
        batch != NULL)
        return 22;
    options.coalesce_parts = KARU_INHERIT;
    options.coalesce_amplification = 0;
    if (karu_client_submit_with(client, NULL, 0, &options, &batch) != KARU_ERR_INVALID ||
        batch != NULL)
        return 23;
    options.coalesce_amplification = KARU_INHERIT;
    options.struct_size = offsetof(karu_submit_options, coalesce_limit);
    if (karu_client_submit_with(client, NULL, 0, &options, &batch) != KARU_OK || batch == NULL)
        return 24;
    karu_batch_free(batch);
    batch = NULL;
    options.struct_size = 0;
    if (karu_client_submit_with(client, NULL, 0, &options, &batch) != KARU_ERR_INVALID ||
        batch != NULL)
        return 25;

    karu_locator_free(locator);
    karu_free(NULL);
    karu_client_free(client);
    karu_config_free(config);
    return 0;
}
