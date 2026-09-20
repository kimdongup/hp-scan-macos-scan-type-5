/*
 * bb_soapht_macos.c - clean-room SOAPHT compatibility plugin for
 * HP LaserJet Pro MFP M127fn on macOS.
 *
 * Current scope (v0.3):
 *   - USB/HPMUD channel: HP-SOAP-SCAN
 *   - Flatbed: Gray/Color, 150/300/600 dpi
 *   - ADF simplex: Gray/Color, 150/300 dpi
 *   - Multi-page ADF batch using one SOAPHT JobId
 *   - JFIF/JPEG transport with HTTP chunking + DIME extraction
 *
 * The public ABI is defined by HPLIP's soaphti.h.
 * This implementation was derived from observable wire behavior and
 * public HPLIP interfaces; it does not contain HP proprietary plugin code.
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sane.h"
#include "saneopts.h"
#include "hpmud.h"
#include "hpip.h"
#include "soaphti.h"

#define SOAPHT_CHANNEL "HP-SOAP-SCAN"
#define SOAPHT_TIMEOUT 45
#define SOAPHT_IMAGE_TIMEOUT 300
#define SOAPHT_MAX_RESPONSE (16u * 1024u * 1024u)

/* M127fn values observed from GetScannerElements. */
#define M127_PLATEN_WIDTH_THOU   8500
#define M127_PLATEN_HEIGHT_THOU 11690
#define M127_MIN_WIDTH_THOU      1920
#define M127_MIN_HEIGHT_THOU     1920

#define M127_ADF_WIDTH_THOU       8500
#define M127_ADF_HEIGHT_THOU     14000
#define M127_ADF_MIN_WIDTH_THOU   1920
#define M127_ADF_MIN_HEIGHT_THOU  1920


struct bb_state {
    int job_id;
    int job_active;
    unsigned char *jpeg;
    size_t jpeg_size;
    size_t jpeg_off;
    int pixels_per_line;
    int lines;
    int bytes_per_line;
};

static const char *xml_prefix =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<SOAP-ENV:Envelope "
    "xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:SOAP-ENC=\"http://www.w3.org/2003/05/soap-encoding\" "
    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
    "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
    "xmlns:wscn=\"http://tempuri.org/wscn.xsd\">"
    "<SOAP-ENV:Body>";

static const char *xml_suffix =
    "</SOAP-ENV:Body></SOAP-ENV:Envelope>";

static int write_all(HPMUD_DEVICE dd, HPMUD_CHANNEL cd,
                     const void *buf, int len, int timeout)
{
    const unsigned char *p = (const unsigned char *)buf;
    int off = 0;
    while (off < len) {
        int wrote = 0;
        enum HPMUD_RESULT r = hpmud_write_channel(dd, cd, p + off,
                                                   len - off, timeout, &wrote);
        if (r != HPMUD_R_OK || wrote <= 0)
            return 1;
        off += wrote;
    }
    return 0;
}

/* Parse an HTTP/1.1 chunked body.
 * return: 1 complete, 0 incomplete, -1 malformed.
 */
static int dechunk_try(const unsigned char *src, size_t src_len,
                       unsigned char **out, size_t *out_len)
{
    size_t p = 0, total = 0;
    unsigned char *dst = NULL;

    while (1) {
        size_t line = p;
        while (line + 1 < src_len &&
               !(src[line] == '\r' && src[line + 1] == '\n'))
            line++;
        if (line + 1 >= src_len) {
            free(dst);
            return 0;
        }

        char hex[32];
        size_t n = line - p;
        if (n == 0 || n >= sizeof(hex)) {
            free(dst);
            return -1;
        }
        memcpy(hex, src + p, n);
        hex[n] = 0;
        char *semi = strchr(hex, ';');
        if (semi) *semi = 0;
        char *endp = NULL;
        unsigned long chunk = strtoul(hex, &endp, 16);
        if (endp == hex) {
            free(dst);
            return -1;
        }
        p = line + 2;

        if (chunk == 0) {
            /* The captured device terminates with 0\r\n\r\n. */
            if (p + 2 > src_len) {
                free(dst);
                return 0;
            }
            if (!(src[p] == '\r' && src[p + 1] == '\n')) {
                free(dst);
                return -1;
            }
            *out = dst;
            *out_len = total;
            return 1;
        }

        if (chunk > SOAPHT_MAX_RESPONSE || p + chunk + 2 > src_len) {
            free(dst);
            return 0;
        }
        if (!(src[p + chunk] == '\r' && src[p + chunk + 1] == '\n')) {
            free(dst);
            return -1;
        }

        if (total + chunk > SOAPHT_MAX_RESPONSE) {
            free(dst);
            return -1;
        }
        unsigned char *tmp = realloc(dst, total + chunk + 1);
        if (!tmp) {
            free(dst);
            return -1;
        }
        dst = tmp;
        memcpy(dst + total, src + p, chunk);
        total += chunk;
        dst[total] = 0;
        p += chunk + 2;
    }
}

static int read_http_response(HPMUD_DEVICE dd, HPMUD_CHANNEL cd,
                              unsigned char **body, size_t *body_len,
                              int timeout)
{
    unsigned char *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;
    size_t hdr_end = 0;

    for (;;) {
        unsigned char tmp[4096];
        int got = 0;
        enum HPMUD_RESULT r = hpmud_read_channel(dd, cd, tmp, sizeof(tmp),
                                                  timeout, &got);
        if (r != HPMUD_R_OK || got < 0) {
            free(raw);
            return 1;
        }
        if (got == 0) {
            free(raw);
            return 1;
        }

        if (raw_len + (size_t)got > SOAPHT_MAX_RESPONSE) {
            free(raw);
            return 1;
        }
        if (raw_len + (size_t)got + 1 > raw_cap) {
            size_t nc = raw_cap ? raw_cap * 2 : 8192;
            while (nc < raw_len + (size_t)got + 1) nc *= 2;
            unsigned char *nr = realloc(raw, nc);
            if (!nr) {
                free(raw);
                return 1;
            }
            raw = nr;
            raw_cap = nc;
        }
        memcpy(raw + raw_len, tmp, (size_t)got);
        raw_len += (size_t)got;
        raw[raw_len] = 0;

        if (!hdr_end) {
            for (size_t i = 0; i + 3 < raw_len; i++) {
                if (raw[i] == '\r' && raw[i+1] == '\n' &&
                    raw[i+2] == '\r' && raw[i+3] == '\n') {
                    hdr_end = i + 4;
                    break;
                }
            }
            if (!hdr_end)
                continue;
            if (raw_len < 12 || memcmp(raw, "HTTP/1.1 2", 10) != 0) {
                free(raw);
                return 1;
            }
        }

        unsigned char *decoded = NULL;
        size_t decoded_len = 0;
        int st = dechunk_try(raw + hdr_end, raw_len - hdr_end,
                             &decoded, &decoded_len);
        if (st == 1) {
            free(raw);
            *body = decoded;
            *body_len = decoded_len;
            return 0;
        }
        if (st < 0) {
            free(raw);
            return 1;
        }
    }
}

static int soap_transaction(struct soap_session *ps,
                            const char *xml,
                            unsigned char **body,
                            size_t *body_len,
                            int timeout)
{
    HPMUD_CHANNEL cd = -1;
    char header[256];
    char chunk_head[32];
    int xml_len = (int)strlen(xml);

    if (hpmud_open_channel(ps->dd, SOAPHT_CHANNEL, &cd) != HPMUD_R_OK)
        return 1;

    int hn = snprintf(header, sizeof(header),
        "POST / HTTP/1.1\r\n"
        "Host: http:0\r\n"
        "User-Agent: gSOAP/2.7\r\n"
        "Content-Type: application/soap+xml; charset=utf-8\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n");

    int cn = snprintf(chunk_head, sizeof(chunk_head), "%x\r\n", xml_len);
    int fail = 0;
    fail |= write_all(ps->dd, cd, header, hn, SOAPHT_TIMEOUT);
    fail |= write_all(ps->dd, cd, chunk_head, cn, 1);
    fail |= write_all(ps->dd, cd, xml, xml_len, 1);
    fail |= write_all(ps->dd, cd, "\r\n0\r\n\r\n", 7, 1);

    if (!fail)
        fail = read_http_response(ps->dd, cd, body, body_len, timeout);

    hpmud_close_channel(ps->dd, cd);
    return fail;
}

static char *make_envelope(const char *inner)
{
    size_t n = strlen(xml_prefix) + strlen(inner) + strlen(xml_suffix) + 1;
    char *s = malloc(n);
    if (!s) return NULL;
    snprintf(s, n, "%s%s%s", xml_prefix, inner, xml_suffix);
    return s;
}

static int tag_int(const char *xml, const char *tag, int *value)
{
    char open[96], close[96];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *a = strstr(xml, open);
    if (!a) return 1;
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b) return 1;
    char tmp[32];
    size_t n = (size_t)(b - a);
    if (n == 0 || n >= sizeof(tmp)) return 1;
    memcpy(tmp, a, n);
    tmp[n] = 0;
    *value = atoi(tmp);
    return 0;
}

static SANE_Fixed thou_to_mm_fixed(int thou)
{
    double mm = ((double)thou / 1000.0) * 25.4;
    return SANE_FIX(mm);
}

static void free_image(struct bb_state *st)
{
    if (!st) return;
    free(st->jpeg);
    st->jpeg = NULL;
    st->jpeg_size = 0;
    st->jpeg_off = 0;
}

static int mm_fixed_to_thou(SANE_Fixed value)
{
    double mm = SANE_UNFIX(value);
    double thou = (mm / 25.4) * 1000.0;

    if (thou < 0.0)
        thou = 0.0;

    return (int)(thou + 0.5);
}

static void setup_capabilities(struct soap_session *ps)
{
    memset(ps->scanModeList, 0, sizeof(ps->scanModeList));
    memset(ps->scanModeMap, 0, sizeof(ps->scanModeMap));
    ps->scanModeList[0] = SANE_VALUE_SCAN_MODE_GRAY;
    ps->scanModeMap[0] = CE_GRAY8;
    ps->scanModeList[1] = SANE_VALUE_SCAN_MODE_COLOR;
    ps->scanModeMap[1] = CE_RGB24;

    memset(ps->inputSourceList, 0, sizeof(ps->inputSourceList));
    memset(ps->inputSourceMap, 0, sizeof(ps->inputSourceMap));
    ps->inputSourceList[0] = "Flatbed";
    ps->inputSourceMap[0] = IS_PLATEN;
    ps->inputSourceList[1] = "ADF";
    ps->inputSourceMap[1] = IS_ADF;

    memset(ps->resolutionList, 0, sizeof(ps->resolutionList));
    memset(ps->platen_resolutionList, 0, sizeof(ps->platen_resolutionList));
    ps->resolutionList[0] = 3;
    ps->resolutionList[1] = 150;
    ps->resolutionList[2] = 300;
    ps->resolutionList[3] = 600;
    ps->platen_resolutionList[0] = 3;
    ps->platen_resolutionList[1] = 150;
    ps->platen_resolutionList[2] = 300;
    ps->platen_resolutionList[3] = 600;

    ps->platen_min_width = thou_to_mm_fixed(M127_MIN_WIDTH_THOU);
    ps->platen_min_height = thou_to_mm_fixed(M127_MIN_HEIGHT_THOU);

    SANE_Fixed platen_max_width =
        thou_to_mm_fixed(M127_PLATEN_WIDTH_THOU);
    SANE_Fixed platen_max_height =
        thou_to_mm_fixed(M127_PLATEN_HEIGHT_THOU);

    ps->platen_tlxRange.min = 0;
    ps->platen_tlxRange.max = platen_max_width;
    ps->platen_tlxRange.quant = 0;
    ps->platen_brxRange = ps->platen_tlxRange;

    ps->platen_tlyRange.min = 0;
    ps->platen_tlyRange.max = platen_max_height;
    ps->platen_tlyRange.quant = 0;
    ps->platen_bryRange = ps->platen_tlyRange;

    memset(&ps->adf_tlxRange, 0, sizeof(ps->adf_tlxRange));
    memset(&ps->adf_tlyRange, 0, sizeof(ps->adf_tlyRange));
    memset(&ps->adf_brxRange, 0, sizeof(ps->adf_brxRange));
    memset(&ps->adf_bryRange, 0, sizeof(ps->adf_bryRange));
    memset(ps->adf_resolutionList, 0, sizeof(ps->adf_resolutionList));

    ps->adf_resolutionList[0] = 2;
    ps->adf_resolutionList[1] = 150;
    ps->adf_resolutionList[2] = 300;

    ps->adf_min_width = thou_to_mm_fixed(M127_ADF_MIN_WIDTH_THOU);
    ps->adf_min_height = thou_to_mm_fixed(M127_ADF_MIN_HEIGHT_THOU);

    SANE_Fixed adf_max_width =
        thou_to_mm_fixed(M127_ADF_WIDTH_THOU);
    SANE_Fixed adf_max_height =
        thou_to_mm_fixed(M127_ADF_HEIGHT_THOU);

    ps->adf_tlxRange.min = 0;
    ps->adf_tlxRange.max = adf_max_width;
    ps->adf_tlxRange.quant = 0;
    ps->adf_brxRange = ps->adf_tlxRange;

    ps->adf_tlyRange.min = 0;
    ps->adf_tlyRange.max = adf_max_height;
    ps->adf_tlyRange.quant = 0;
    ps->adf_bryRange = ps->adf_tlyRange;

    ps->jpegQualityRange.min = 0;
    ps->jpegQualityRange.max = 100;
    ps->jpegQualityRange.quant = 0;
}

__attribute__((visibility("default")))
int bb_open(struct soap_session *ps)
{
    if (!ps) return 1;
    struct bb_state *st = calloc(1, sizeof(*st));
    if (!st) return 1;
    ps->bb_session = st;

    /* Probe the real scanner once, matching the proprietary plugin's flow. */
    char *xml = make_envelope(
        "<wscn:GetScannerElements></wscn:GetScannerElements>");
    if (!xml) return 1;
    unsigned char *resp = NULL;
    size_t resp_len = 0;
    int rc = soap_transaction(ps, xml, &resp, &resp_len, SOAPHT_TIMEOUT);
    free(xml);
    if (rc) {
        free(st);
        ps->bb_session = NULL;
        return 1;
    }

    /* Minimal sanity check: this is the SOAPHT scanner we expect. */
    if (!strstr((char *)resp, "<ScannerConfiguration>") ||
        !strstr((char *)resp, "<FlatbedSupported>true</FlatbedSupported>")) {
        free(resp);
        free(st);
        ps->bb_session = NULL;
        return 1;
    }
    free(resp);

    setup_capabilities(ps);
    return 0;
}

__attribute__((visibility("default")))
int bb_close(struct soap_session *ps)
{
    if (!ps) return 0;
    struct bb_state *st = (struct bb_state *)ps->bb_session;
    if (st) {
        free_image(st);
        free(st);
    }
    ps->bb_session = NULL;
    return 0;
}

__attribute__((visibility("default")))
int bb_get_parameters(struct soap_session *ps, SANE_Parameters *pp,
                      int scan_started)
{
    (void)scan_started;
    if (!ps || !pp) return 1;

    struct bb_state *st = (struct bb_state *)ps->bb_session;

    memset(pp, 0, sizeof(*pp));

    const int is_rgb = (ps->currentScanMode == CE_RGB24);
    int dpi = ps->currentResolution;

    if (dpi != 150 && dpi != 300 && dpi != 600)
        dpi = 300;

    pp->format = is_rgb ? SANE_FRAME_RGB : SANE_FRAME_GRAY;
    pp->last_frame = SANE_TRUE;
    pp->depth = 8;

    if (st && st->pixels_per_line > 0 && st->lines > 0) {
        pp->pixels_per_line = st->pixels_per_line;
        pp->lines = st->lines;

        /*
         * IMPORTANT: Do not trust MediaFrontImageInfo/BytesPerLine for JFIF.
         *
         * On the M127fn the SOAP response reports inconsistent BytesPerLine
         * values for compressed JPEG jobs.  Observed examples from the
         * proprietary Linux plugin:
         *
         *   150 dpi Gray: PixelsPerLine=1274, BytesPerLine=7647
         *   300 dpi RGB : PixelsPerLine=2549, BytesPerLine=5099
         *   600 dpi Gray: PixelsPerLine=5099, BytesPerLine=1274
         *
         * The actual reconstructed JPEGs are respectively:
         *   1274x1753 L, 2549x3506 RGB, 5099x7013 L.
         *
         * HPLIP's image processor needs the uncompressed output row size
         * for its best-guess traits, not that unreliable SOAP field.
         */
        pp->bytes_per_line =
            st->pixels_per_line * (is_rgb ? 3 : 1);
    } else {
        /*
         * Best guess before the job starts.
         * Width/height are thousandths of an inch and the M127fn truncates
         * to integer pixels.
         */
        pp->pixels_per_line =
            (8499 * dpi) / 1000;
        pp->lines =
            (11689 * dpi) / 1000;
        pp->bytes_per_line =
            pp->pixels_per_line * (is_rgb ? 3 : 1);
    }

    return 0;
}

__attribute__((visibility("default")))
int bb_is_paper_in_adf(struct soap_session *ps)
{
    if (!ps)
        return -1;

    char *xml = make_envelope(
        "<wscn:GetScannerElements></wscn:GetScannerElements>");
    if (!xml)
        return -1;

    unsigned char *resp = NULL;
    size_t resp_len = 0;
    int rc = soap_transaction(
        ps, xml, &resp, &resp_len, SOAPHT_TIMEOUT);

    free(xml);

    if (rc || !resp) {
        free(resp);
        return -1;
    }

    int result = -1;
    const char *paper = strstr((char *)resp, "PaperInADF");
    if (paper) {
        const char *value = strchr(paper, '>');
        if (value) {
            value++;
            if (strncmp(value, "true", 4) == 0 ||
                strncmp(value, "1", 1) == 0) {
                result = 1;
            } else if (strncmp(value, "false", 5) == 0 ||
                       strncmp(value, "0", 1) == 0) {
                result = 0;
            }
        }
    }

    free(resp);
    return result;
}

__attribute__((visibility("default")))
int bb_start_scan(struct soap_session *ps)
{
    if (!ps || !ps->bb_session)
        return 1;

    struct bb_state *st = (struct bb_state *)ps->bb_session;
    const int is_adf =
        (ps->currentInputSource == IS_ADF ||
         ps->currentInputSource == IS_ADF_DUPLEX);

    if (ps->currentInputSource != IS_PLATEN &&
        ps->currentInputSource != IS_ADF) {
        return 1;
    }

    /*
     * The M127fn supports simplex ADF only. Keep the same SOAPHT JobId
     * across ADF pages; each subsequent RetrieveImage returns the next page.
     */
    free_image(st);
    st->pixels_per_line = 0;
    st->lines = 0;
    st->bytes_per_line = 0;

    if (is_adf && st->job_active && st->job_id > 0)
        return 0;

    st->job_id = 0;
    st->job_active = 0;

    if (ps->currentCompression != SF_JFIF)
        ps->currentCompression = SF_JFIF;

    if (is_adf) {
        if (ps->currentResolution != 150 &&
            ps->currentResolution != 300) {
            ps->currentResolution = 300;
        }
    } else {
        if (ps->currentResolution != 150 &&
            ps->currentResolution != 300 &&
            ps->currentResolution != 600) {
            ps->currentResolution = 300;
        }
    }

    const char *color_processing;
    switch (ps->currentScanMode) {
        case CE_RGB24:
            color_processing = "RGB24";
            break;
        case CE_GRAY8:
            color_processing = "GrayScale8";
            break;
        default:
            return 1;
    }

const char *source_name = is_adf ? "ADF" : "Platen";

int x_offset = mm_fixed_to_thou(ps->effectiveTlx);
int y_offset = mm_fixed_to_thou(ps->effectiveTly);

int media_width =
    mm_fixed_to_thou(ps->effectiveBrx - ps->effectiveTlx);

int media_height =
    mm_fixed_to_thou(ps->effectiveBry - ps->effectiveTly);

/* Fallback for uninitialized/invalid geometry. */
if (media_width <= 0)
    media_width = 8499;

if (media_height <= 0)
    media_height = 11689;

/* Clamp to scanner hardware limits. */
if (media_width > M127_ADF_WIDTH_THOU)
    media_width = M127_ADF_WIDTH_THOU;

if (is_adf) {
    /*
     * HPLIP initializes the ADF geometry to its advertised maximum
     * 14-inch height. That produces an unnecessary blank tail for the
     * normal default scan, so retain the tested A4-ish default.
     */
    if (media_height >= M127_ADF_HEIGHT_THOU - 1)
        media_height = 11689;

    if (media_height > M127_ADF_HEIGHT_THOU)
        media_height = M127_ADF_HEIGHT_THOU;
} else {
    if (media_height > M127_PLATEN_HEIGHT_THOU)
        media_height = M127_PLATEN_HEIGHT_THOU;
}

    char inner[4096];
    int n = snprintf(
        inner, sizeof(inner),
        "<wscn:CreateScanJobRequest>"
        "<ScanIdentifier></ScanIdentifier>"
        "<ScanTicket><JobDescription></JobDescription>"
        "<DocumentParameters>"
        "<Format>jfif</Format>"
        "<CompressionQualityFactor>0</CompressionQualityFactor>"
        "<ImagesToTransfer>0</ImagesToTransfer>"
        "<InputSource>%s</InputSource>"
        "<ContentType>Auto</ContentType>"
        "<InputSize><InputMediaSize>"
        "<Width>%d</Width><Height>%d</Height>"
        "</InputMediaSize>"
        "<DocumentSizeAutoDetect>false</DocumentSizeAutoDetect>"
        "</InputSize>"
        "<Exposure><AutoExposure>false</AutoExposure>"
        "<ExposureSettings>"
        "<Contrast>%d</Contrast>"
        "<Brightness>%d</Brightness>"
        "</ExposureSettings></Exposure>"
        "<MediaSides><MediaFront>"
        "<ScanRegion>"
        "<ScanRegionXOffset>%d</ScanRegionXOffset>"
        "<ScanRegionYOffset>%d</ScanRegionYOffset>"
        "<ScanRegionWidth>%d</ScanRegionWidth>"
        "<ScanRegionHeight>%d</ScanRegionHeight>"
        "</ScanRegion>"
        "<ColorProcessing>%s</ColorProcessing>"
        "<Resolution><Width>%d</Width><Height>%d</Height></Resolution>"
        "</MediaFront></MediaSides>"
        "</DocumentParameters>"
        "<RetrieveImageTimeout>300</RetrieveImageTimeout>"
        "<ScanManufacturingParameters>"
        "<DisableImageProcessing>false</DisableImageProcessing>"
        "</ScanManufacturingParameters>"
        "</ScanTicket>"
        "</wscn:CreateScanJobRequest>",
        source_name,
        media_width,
        media_height,
        ps->currentContrast,
        ps->currentBrightness,
       x_offset,
       y_offset,
        media_width,
        media_height,
        color_processing,
        ps->currentResolution,
        ps->currentResolution);

    if (n < 0 || (size_t)n >= sizeof(inner))
        return 1;

    char *xml = make_envelope(inner);
    if (!xml)
        return 1;

    unsigned char *resp = NULL;
    size_t resp_len = 0;
    int rc = soap_transaction(
        ps, xml, &resp, &resp_len, SOAPHT_TIMEOUT);

    free(xml);

    if (rc || !resp) {
        free(resp);
        return 1;
    }

    if (tag_int((char *)resp, "JobId", &st->job_id) ||
        tag_int((char *)resp, "PixelsPerLine", &st->pixels_per_line) ||
        tag_int((char *)resp, "NumberOfLines", &st->lines) ||
        tag_int((char *)resp, "BytesPerLine", &st->bytes_per_line)) {
        free(resp);
        return 1;
    }

    free(resp);
    st->job_active = 1;
    return 0;
}

static uint16_t be16(const unsigned char *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           (uint32_t)p[3];
}

static size_t pad4(size_t n)
{
    return (n + 3u) & ~3u;
}

static int extract_dime_jpeg(const unsigned char *body,
                             size_t body_len,
                             unsigned char **jpeg,
                             size_t *jpeg_len)
{
    size_t pos = 0;
    unsigned char *out = NULL;
    size_t out_len = 0;
    int image_started = 0;

    while (pos + 12 <= body_len) {
        const unsigned char *h = body + pos;

        /*
         * DIME 12-byte header:
         *   byte 0    version + MB/ME/CF
         *   byte 1    type format
         *   byte 2-3  options length
         *   byte 4-5  ID length
         *   byte 6-7  type length
         *   byte 8-11 data length
         */
        unsigned char flags = h[0];
        int cf = (flags & 0x01) != 0;

        uint16_t options_len = be16(h + 2);
        uint16_t id_len = be16(h + 4);
        uint16_t type_len = be16(h + 6);
        uint32_t data_len = be32(h + 8);

        pos += 12;

        size_t options_pos = pos;
        size_t id_pos = options_pos + pad4(options_len);
        size_t type_pos = id_pos + pad4(id_len);
        size_t data_pos = type_pos + pad4(type_len);
        size_t next_pos = data_pos + pad4(data_len);

        if (next_pos > body_len)
            goto fail;

        if (!image_started &&
            type_len == strlen("image/jfif") &&
            memcmp(body + type_pos, "image/jfif", type_len) == 0) {
            image_started = 1;
        }

        if (image_started && data_len > 0) {
            unsigned char *tmp = realloc(out, out_len + data_len);
            if (!tmp)
                goto fail;

            out = tmp;
            memcpy(out + out_len, body + data_pos, data_len);
            out_len += data_len;
        }

        if (image_started && !cf) {
            if (out_len < 4 ||
                out[0] != 0xff ||
                out[1] != 0xd8 ||
                out[out_len - 2] != 0xff ||
                out[out_len - 1] != 0xd9) {
                goto fail;
            }

            *jpeg = out;
            *jpeg_len = out_len;
            return 0;
        }

        pos = next_pos;
    }

fail:
    free(out);
    return 1;
}

static int fetch_jpeg(struct soap_session *ps, struct bb_state *st)
{
    char inner[768];
    int n = snprintf(
        inner, sizeof(inner),
        "<wscn:RetrieveImageRequest>"
        "<JobId>%d</JobId><JobToken></JobToken>"
        "<DocumentDescription></DocumentDescription>"
        "</wscn:RetrieveImageRequest>",
        st->job_id);

    if (n < 0 || (size_t)n >= sizeof(inner))
        return 1;

    char *xml = make_envelope(inner);
    if (!xml)
        return 1;

    unsigned char *body = NULL;
    size_t body_len = 0;
    int rc = soap_transaction(
        ps, xml, &body, &body_len, SOAPHT_IMAGE_TIMEOUT);

    free(xml);

    if (rc) {
        free(body);
        return 1;
    }

    unsigned char *jpeg = NULL;
    size_t jpeg_len = 0;

    if (extract_dime_jpeg(body, body_len, &jpeg, &jpeg_len)) {
        free(body);
        return 1;
    }

    free(body);

    st->jpeg = jpeg;
    st->jpeg_size = jpeg_len;
    st->jpeg_off = 0;
    return 0;
}

__attribute__((visibility("default")))
int bb_get_image_data(struct soap_session *ps, int max_length)
{
    if (!ps || !ps->bb_session) return 1;
    struct bb_state *st = (struct bb_state *)ps->bb_session;

    /* soapht.c may call us while unconsumed input remains. */
    if (ps->cnt > 0)
        return 0;

    if (!st->jpeg) {
        if (!st->job_active || fetch_jpeg(ps, st))
            return 1;
    }

    if (st->jpeg_off >= st->jpeg_size) {
        ps->index = 0;
        ps->cnt = 0;
        return 0;
    }

    size_t n = st->jpeg_size - st->jpeg_off;
    if (n > sizeof(ps->buf)) n = sizeof(ps->buf);
    if (max_length > 0 && n > (size_t)max_length) n = (size_t)max_length;

    memcpy(ps->buf, st->jpeg + st->jpeg_off, n);
    st->jpeg_off += n;
    ps->index = 0;
    ps->cnt = (int)n;
    return 0;
}

__attribute__((visibility("default")))
int bb_end_page(struct soap_session *ps, int io_error)
{
    (void)io_error;

    if (!ps || !ps->bb_session)
        return 0;

    struct bb_state *st = (struct bb_state *)ps->bb_session;

    /*
     * Release only the current page image. For an ADF batch the SOAPHT
     * JobId remains active so the next sane_start() can retrieve the next
     * page from the same scan job.
     */
    free_image(st);
    ps->index = 0;
    ps->cnt = 0;
    return 0;
}

static int cancel_job(struct soap_session *ps, struct bb_state *st)
{
    if (!st || !st->job_active || st->job_id <= 0)
        return 0;

    char inner[768];
    snprintf(inner, sizeof(inner),
        "<wscn:CancelJobRequest>"
        "<JobId>%d</JobId><JobToken></JobToken>"
        "<DocumentDescription></DocumentDescription>"
        "</wscn:CancelJobRequest>", st->job_id);
    char *xml = make_envelope(inner);
    if (!xml) return 1;

    unsigned char *resp = NULL;
    size_t resp_len = 0;
    int rc = soap_transaction(ps, xml, &resp, &resp_len, SOAPHT_TIMEOUT);
    free(xml);
    free(resp);
    st->job_active = 0;
    st->job_id = 0;
    return rc;
}

__attribute__((visibility("default")))
int bb_end_scan(struct soap_session *ps, int io_error)
{
    (void)io_error;
    if (!ps || !ps->bb_session) return 0;
    struct bb_state *st = (struct bb_state *)ps->bb_session;
    int rc = cancel_job(ps, st);
    free_image(st);
    ps->index = 0;
    ps->cnt = 0;
    return rc;
}
