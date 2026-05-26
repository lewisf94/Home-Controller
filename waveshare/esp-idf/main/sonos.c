/* See sonos.h. UPnP/SOAP control of a Sonos speaker on the local network. */

#include "sonos.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "sonos";

#define AVT_SVC   "urn:schemas-upnp-org:service:AVTransport:1"
#define RC_SVC    "urn:schemas-upnp-org:service:RenderingControl:1"
#define AVT_PATH  "/MediaRenderer/AVTransport/Control"
#define RC_PATH   "/MediaRenderer/RenderingControl/Control"

/* Build + send one SOAP action. `inner` is the action-specific XML inside the
 * <u:Action> element, after the always-present <InstanceID>0</InstanceID>. */
static bool soap_post(const char *host, const char *path, const char *svc,
                      const char *action, const char *inner)
{
    if (!host || !host[0]) return false;

    char url[96];
    snprintf(url, sizeof url, "http://%s:1400%s", host, path);

    char soapaction[160];
    snprintf(soapaction, sizeof soapaction, "\"%s#%s\"", svc, action);

    char body[512];
    int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%s xmlns:u=\"%s\"><InstanceID>0</InstanceID>%s</u:%s></s:Body>"
        "</s:Envelope>",
        action, svc, inner ? inner : "", action);
    if (n <= 0 || n >= (int)sizeof body) return false;

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    esp_http_client_set_header(c, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(c, "SOAPAction", soapaction);
    esp_http_client_set_post_field(c, body, n);

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    bool ok = (err == ESP_OK && status == 200);
    if (!ok) ESP_LOGW(TAG, "%s -> err=%d status=%d", action, (int)err, status);
    return ok;
}

bool sonos_play(const char *host)     { return soap_post(host, AVT_PATH, AVT_SVC, "Play", "<Speed>1</Speed>"); }
bool sonos_pause(const char *host)    { return soap_post(host, AVT_PATH, AVT_SVC, "Pause", ""); }
bool sonos_next(const char *host)     { return soap_post(host, AVT_PATH, AVT_SVC, "Next", ""); }
bool sonos_previous(const char *host) { return soap_post(host, AVT_PATH, AVT_SVC, "Previous", ""); }

bool sonos_seek_ms(const char *host, uint32_t position_ms)
{
    uint32_t s = position_ms / 1000;
    char inner[96];
    snprintf(inner, sizeof inner,
             "<Unit>REL_TIME</Unit><Target>%lu:%02lu:%02lu</Target>",
             (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60),
             (unsigned long)(s % 60));
    return soap_post(host, AVT_PATH, AVT_SVC, "Seek", inner);
}

bool sonos_set_volume(const char *host, int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    char inner[96];
    snprintf(inner, sizeof inner,
             "<Channel>Master</Channel><DesiredVolume>%d</DesiredVolume>", pct);
    return soap_post(host, RC_PATH, RC_SVC, "SetVolume", inner);
}
