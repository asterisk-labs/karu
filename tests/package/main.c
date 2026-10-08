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
    const karu_download_options download_options = KARU_DOWNLOAD_OPTIONS_INIT;
    karu_download_result result = KARU_DOWNLOAD_RESULT_INIT;
    if (download_options.chunk_size != KARU_DOWNLOAD_CHUNK_SIZE_DEFAULT ||
        result.struct_size != sizeof(result))
        return 4;
    if (karu_client_download(client, NULL, "unused", &download_options, &result) !=
        KARU_ERR_INVALID)
        return 5;
    karu_client_free(client);
    karu_config_free(config);
    return 0;
}
