#include <karu/karu.h>

int main(void) {
    karu_config* config = NULL;
    karu_client* client = NULL;
    if (karu_config_create_empty(&config) != KARU_OK)
        return 1;
    if (karu_client_create(config, &client) != KARU_OK) {
        karu_config_free(config);
        return 2;
    }
    const karu_submit_options options = KARU_SUBMIT_OPTIONS_INIT;
    if (karu_client_fetch_with(client, NULL, 0, &options) != KARU_OK)
        return 3;
    karu_client_free(client);
    karu_config_free(config);
    return 0;
}
