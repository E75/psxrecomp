/* Host harness for asset_prepare.c (offline, no game boot).
 *
 *   test_asset_prepare <disc.cue|bin> <root-dir> <expected-raw>
 *
 * Produces assets/str/WE2002.STR.raw under <root-dir> from the user's own
 * disc, then compares it byte-for-byte with <expected-raw> (the file the
 * Python tool wrote). Prints the report so the JSON shape is visible.
 */
#include "asset_prepare.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_files(const char* a, const char* b) {
    FILE* fa = fopen(a, "rb");
    FILE* fb = fopen(b, "rb");
    unsigned char ba[65536], bb[65536];
    long long off = 0;
    int same = 1;
    if (!fa || !fb) {
        fprintf(stderr, "compare: cannot open %s\n", !fa ? a : b);
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        size_t i;
        if (na != nb) {
            fprintf(stderr, "compare: length differs at byte %lld\n", off);
            same = 0;
            break;
        }
        for (i = 0; i < na; ++i) {
            if (ba[i] != bb[i]) {
                fprintf(stderr,
                        "compare: first difference at byte %lld (0x%llx): "
                        "%02x != %02x\n",
                        off + (long long)i, off + (long long)i, ba[i], bb[i]);
                same = 0;
                break;
            }
        }
        if (!same || na == 0) break;
        off += (long long)na;
    }
    fclose(fa);
    fclose(fb);
    if (same) printf("compare: identical, %lld bytes\n", off);
    return same;
}

int main(int argc, char** argv) {
    AssetPrepareOptions opt;
    AssetPrepareResult res;
    int rc;
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <disc.cue|bin> <root-dir> <expected-raw>\n",
                argv[0]);
        return 2;
    }
    memset(&opt, 0, sizeof(opt));
    memset(&res, 0, sizeof(res));
    opt.disc_path = argv[1];
    opt.root_dir = argv[2];
    opt.force = (argc > 4 && strcmp(argv[4], "noforce") == 0) ? 0 : 1;
    rc = asset_prepare_run(&opt, &res);
    printf("asset_prepare_run -> %d (prepared=%d present=%d python_missing=%d)\n",
           rc, res.prepared, res.present, res.missing_python);
    if (rc < 0) {
        fprintf(stderr, "error: %s\n", asset_prepare_last_error());
        return 1;
    }
    printf("str_raw: %s\nreport:  %s\n", res.str_raw_path, res.report_path);
    {
        FILE* f = fopen(res.report_path, "rb");
        char line[512];
        if (!f) {
            fprintf(stderr, "error: no report at %s\n", res.report_path);
            return 1;
        }
        printf("---- prepare_report.json ----\n");
        while (fgets(line, sizeof(line), f)) fputs(line, stdout);
        printf("-----------------------------\n");
        fclose(f);
    }
    if (res.prepared != 1) {
        if (opt.force) {
            fprintf(stderr, "error: STR raw was not produced\n");
            return 1;
        }
        printf("no artefact produced (prepare_on_start would be a no-op)\n");
        return 0;
    }
    if (!compare_files(res.str_raw_path, argv[3])) return 1;
    return 0;
}
