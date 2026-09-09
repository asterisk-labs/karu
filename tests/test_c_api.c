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
    karu_locator_free(locator);
    karu_free(NULL);
    karu_client_free(client);
    karu_config_free(config);
    return 0;
}
