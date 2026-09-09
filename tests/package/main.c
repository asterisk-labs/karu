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
    karu_client_free(client);
    karu_config_free(config);
    return 0;
}
