/*
 * NXP i.MX 95 Neutron NPU - runner subprocess backend (M3).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Plugs into the NeutronBackendOps seam (include/hw/misc/imx95_neutron_compute.h)
 * and turns each mailbox RUN into a real inference by shelling out to the
 * operator-supplied "neutron-runner" ELF - a file-in / file-out CLI (from the
 * eIQ Neutron SDK, brought by the user) that embeds the Neutron Runtime's
 * bit-exact golden kernels. The two-track split (upstream-clean front end vs
 * this downstream runner) is in docs/neutron-two-track-roadmap.md; the fixture
 * manifest schema is the JSON in hw/misc/imx95_neutron_fixtures.mobilenet_v1.json.
 *
 * On each RUN:
 *   1. Look up a fixture keyed by (microcode_offset, tensor_count) in the JSON
 *      manifest to get: absolute path to the .tflite, (input_offset,
 *      input_size), (output_offset, output_size).
 *   2. Stage a per-job scratch directory: DMA-read input_size bytes from
 *      carveout+input_offset into inputs/0000.bin, symlink model.tflite.
 *   3. Exec neutron-runner, wait up to timeout_ms.
 *   4. Read outputs.bin, DMA-write output_size bytes to carveout+output_offset.
 *   5. Report retcode=DONE, guest_errcode=0. On any failure we still report
 *      retcode=DONE (the driver's completion gate depends on it) but a
 *      recognisable NEUTRON_RUNNER_ERR_* in MBOX1 - same "honest fault" idea
 *      the null backend uses.
 *
 * KERNELS and other non-RUN commands are ACKed with DONE / error_code=0 (the
 * runner has no equivalent; kernels are already inside the .tflite).
 *
 * This backend does NOT use worker threads today - it is called from the same
 * QEMUTimer callback the null backend uses, under the BQL, so DMA calls just
 * use dma_memory_read/write on the passed AddressSpace. If subprocess latency
 * becomes a problem, moving the run() call into a QEMU worker (per plan sec. 6
 * "Runner startup cost") is a drop-in change - the seam already carries
 * everything a worker would need.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/dma.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/imx95_neutron_compute.h"

#include <glib.h>
#include <gio/gio.h>

#define NEUTRON_RUNNER_DEFAULT_TIMEOUT_MS  10000
#define NEUTRON_RUNNER_DONE                0xAD0

/* One row of the fixture manifest. */
typedef struct RunnerFixture {
    /* Lookup keys - matched against the guest-posted RUN args. */
    uint32_t microcode_offset;
    uint32_t tensor_count;

    /* Host-side data. */
    char    *name;              /* human-readable label */
    char    *tflite_path;       /* absolute path to the .tflite */

    /* Carveout spans, driver-observable. */
    uint32_t input_offset;
    uint32_t input_size;
    uint32_t output_offset;
    uint32_t output_size;
} RunnerFixture;

typedef struct RunnerCtx {
    /* Copies of the QOM strings (cfg storage is caller-owned). */
    char    *runner_path;
    char    *fixtures_path;
    char    *scratch_dir;
    uint32_t timeout_ms;
    bool     keep_scratch;

    /* Loaded manifest. */
    RunnerFixture *fixtures;
    size_t         num_fixtures;

    /* Monotonic job counter for scratch dir names. */
    uint64_t job_seq;
} RunnerCtx;

/* ---- fixture manifest loader ---- */

static bool runner_load_fixtures(RunnerCtx *ctx, Error **errp)
{
    g_autofree gchar *raw = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    QObject *root = NULL;
    QList *entries = NULL;
    size_t n;

    if (!g_file_get_contents(ctx->fixtures_path, &raw, &len, &gerr)) {
        error_setg(errp, "neutron-runner: cannot read fixtures '%s': %s",
                   ctx->fixtures_path, gerr ? gerr->message : "unknown");
        if (gerr) {
            g_error_free(gerr);
        }
        return false;
    }

    root = qobject_from_json(raw, errp);
    if (!root) {
        return false;
    }

    /*
     * Accept either a top-level list of fixtures, or a { "fixtures": [...] }
     * dict - the latter gives us room to grow schema-wide metadata.
     */
    if (qobject_type(root) == QTYPE_QLIST) {
        entries = qobject_to(QList, root);
    } else if (qobject_type(root) == QTYPE_QDICT) {
        QDict *d = qobject_to(QDict, root);
        entries = qdict_get_qlist(d, "fixtures");
        if (!entries) {
            error_setg(errp, "neutron-runner: manifest '%s' has no 'fixtures'"
                       " list", ctx->fixtures_path);
            qobject_unref(root);
            return false;
        }
    } else {
        error_setg(errp, "neutron-runner: manifest '%s' is not JSON list/obj",
                   ctx->fixtures_path);
        qobject_unref(root);
        return false;
    }

    n = qlist_size(entries);
    ctx->fixtures = g_new0(RunnerFixture, n);
    ctx->num_fixtures = 0;
    for (size_t i = 0; i < n; i++) {
        QObject *item = qlist_entry_obj(qlist_first(entries));
        QDict *e;
        RunnerFixture *f;
        const char *s;

        /*
         * qlist_first + advance would need iterator plumbing; the simpler
         * portable path is qlist_pop() which moves items off the head. That
         * mutates entries but we drop the parent right after this loop.
         */
        item = qlist_pop(entries);
        if (!item || qobject_type(item) != QTYPE_QDICT) {
            error_setg(errp, "neutron-runner: fixture #%zu is not a JSON obj",
                       i);
            qobject_unref(item);
            qobject_unref(root);
            return false;
        }
        e = qobject_to(QDict, item);
        f = &ctx->fixtures[ctx->num_fixtures];

        s = qdict_get_try_str(e, "name");
        f->name = g_strdup(s ? s : "unnamed");

        s = qdict_get_try_str(e, "tflite_path");
        if (!s) {
            error_setg(errp, "neutron-runner: fixture '%s' missing tflite_path",
                       f->name);
            g_free(f->name);
            qobject_unref(item);
            qobject_unref(root);
            return false;
        }
        f->tflite_path = g_strdup(s);

        f->microcode_offset = qdict_get_try_int(e, "microcode_offset", 0);
        f->tensor_count     = qdict_get_try_int(e, "tensor_count", 0);
        f->input_offset     = qdict_get_try_int(e, "input_offset", 0);
        f->input_size       = qdict_get_try_int(e, "input_size", 0);
        f->output_offset    = qdict_get_try_int(e, "output_offset", 0);
        f->output_size      = qdict_get_try_int(e, "output_size", 0);

        ctx->num_fixtures++;
        qobject_unref(item);
    }

    qobject_unref(root);
    return true;
}

static const RunnerFixture *runner_match(const RunnerCtx *ctx,
                                         const NeutronComputeReq *req)
{
    for (size_t i = 0; i < ctx->num_fixtures; i++) {
        const RunnerFixture *f = &ctx->fixtures[i];
        if (f->microcode_offset == req->microcode_offset &&
            f->tensor_count     == req->tensor_count) {
            return f;
        }
    }
    return NULL;
}

/* ---- one job ---- */

/*
 * DMA-read input, exec neutron-runner, DMA-write output. Returns 0 on success,
 * else a NEUTRON_RUNNER_ERR_* value. Failure paths log via qemu_log_mask.
 */
static uint32_t runner_run_one(RunnerCtx *ctx, const RunnerFixture *fx,
                               const NeutronComputeReq *req)
{
    g_autofree gchar *job_dir = NULL;
    g_autofree gchar *inputs_dir = NULL;
    g_autofree gchar *input_path = NULL;
    g_autofree gchar *model_path = NULL;
    g_autofree gchar *output_path = NULL;
    g_autofree gchar *stderr_buf = NULL;
    g_autofree void  *input_buf = NULL;
    g_autofree gchar *output_buf = NULL;
    gsize output_len = 0;
    GError *gerr = NULL;
    GSubprocess *proc = NULL;
    uint32_t err = 0;
    uint64_t seq = ++ctx->job_seq;
    const char *scratch =
        ctx->scratch_dir && *ctx->scratch_dir ? ctx->scratch_dir : g_get_tmp_dir();

    job_dir = g_strdup_printf("%s/qemu-neutron-job-%d-%" PRIu64,
                              scratch, (int)getpid(), seq);
    inputs_dir = g_strdup_printf("%s/inputs", job_dir);
    input_path = g_strdup_printf("%s/0000.bin", inputs_dir);
    model_path = g_strdup_printf("%s/model.tflite", job_dir);
    output_path = g_strdup_printf("%s/outputs.bin", job_dir);

    if (g_mkdir_with_parents(inputs_dir, 0700) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: mkdir '%s' failed: %s\n",
                      inputs_dir, g_strerror(errno));
        return NEUTRON_RUNNER_ERR_EXEC_FAILED;
    }

    /* Stage inputs from guest DMA. */
    input_buf = g_malloc(fx->input_size);
    if (dma_memory_read(req->as, req->carveout_base + fx->input_offset,
                        input_buf, fx->input_size,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: dma_memory_read %u B at 0x%" PRIx64
                      " failed\n", fx->input_size,
                      req->carveout_base + fx->input_offset);
        err = NEUTRON_RUNNER_ERR_DMA_FAILED;
        goto out;
    }
    if (!g_file_set_contents(input_path, input_buf, fx->input_size, &gerr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: write '%s' failed: %s\n",
                      input_path, gerr ? gerr->message : "unknown");
        if (gerr) {
            g_error_free(gerr);
            gerr = NULL;
        }
        err = NEUTRON_RUNNER_ERR_EXEC_FAILED;
        goto out;
    }

    /* Symlink model into place so --input can be a stable relative name. */
    if (symlink(fx->tflite_path, model_path) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: symlink '%s' -> '%s' failed: %s\n",
                      fx->tflite_path, model_path, g_strerror(errno));
        err = NEUTRON_RUNNER_ERR_EXEC_FAILED;
        goto out;
    }

    /* Exec: neutron-runner --input <> --dataset <> --target imx95
     *                     --use-neutron-runtime=true --merge-outputs
     *                     --output-results outputs.bin */
    proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                            G_SUBPROCESS_FLAGS_STDERR_PIPE,
                            &gerr,
                            ctx->runner_path,
                            "--input",  model_path,
                            "--dataset", inputs_dir,
                            "--target", "imx95",
                            "--use-neutron-runtime=true",
                            "--merge-outputs",
                            "--output-results", output_path,
                            NULL);
    if (!proc) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: spawn '%s' failed: %s\n",
                      ctx->runner_path, gerr ? gerr->message : "unknown");
        if (gerr) {
            g_error_free(gerr);
            gerr = NULL;
        }
        err = NEUTRON_RUNNER_ERR_EXEC_FAILED;
        goto out;
    }

    /*
     * Bound the wait. GSubprocess has no builtin timeout, so we poll wait()
     * against a wall-clock deadline; when we blow it we force_exit and set
     * the timeout errcode. Poll interval 20 ms keeps overhead negligible.
     */
    {
        gint64 deadline_us = g_get_monotonic_time() +
            (gint64)ctx->timeout_ms * 1000;
        bool exited = false;
        while (g_get_monotonic_time() < deadline_us) {
            if (g_subprocess_get_if_exited(proc)) {
                exited = true;
                break;
            }
            g_usleep(20 * 1000);
        }
        if (!exited) {
            g_subprocess_force_exit(proc);
            g_subprocess_wait(proc, NULL, NULL);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "neutron-runner: subprocess timed out after %u ms\n",
                          ctx->timeout_ms);
            err = NEUTRON_RUNNER_ERR_TIMEOUT;
            goto out;
        }
    }

    if (!g_subprocess_get_successful(proc)) {
        /*
         * Slurp stderr so the log entry is actionable (the runner's own
         * error text is usually plenty).
         */
        GBytes *b = NULL;
        g_subprocess_communicate(proc, NULL, NULL, NULL, &b, NULL);
        if (b) {
            gsize sl = 0;
            const char *sd = g_bytes_get_data(b, &sl);
            stderr_buf = g_strndup(sd, sl);
            g_bytes_unref(b);
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: subprocess exited non-zero (%d): %s\n",
                      g_subprocess_get_exit_status(proc),
                      stderr_buf ? stderr_buf : "(no stderr)");
        err = NEUTRON_RUNNER_ERR_EXEC_FAILED;
        goto out;
    }

    /* Slurp output, size-check, DMA back into guest. */
    if (!g_file_get_contents(output_path, &output_buf, &output_len, &gerr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: read '%s' failed: %s\n",
                      output_path, gerr ? gerr->message : "unknown");
        if (gerr) {
            g_error_free(gerr);
            gerr = NULL;
        }
        err = NEUTRON_RUNNER_ERR_EXEC_FAILED;
        goto out;
    }
    if (output_len != fx->output_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: output size %zu != expected %u for '%s'\n",
                      output_len, fx->output_size, fx->name);
        err = NEUTRON_RUNNER_ERR_SIZE_MISMATCH;
        goto out;
    }
    if (dma_memory_write(req->as, req->carveout_base + fx->output_offset,
                         output_buf, fx->output_size,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: dma_memory_write %u B at 0x%" PRIx64
                      " failed\n", fx->output_size,
                      req->carveout_base + fx->output_offset);
        err = NEUTRON_RUNNER_ERR_DMA_FAILED;
        goto out;
    }

out:
    if (proc) {
        g_object_unref(proc);
    }
    /* Retain scratch on failure or when the operator asked us to. */
    if (err == 0 && !ctx->keep_scratch) {
        /*
         * No portable recursive rmdir in glib without gio; use a tiny inline
         * walk. We only ever create three known files, so keep it simple.
         */
        unlink(input_path);
        unlink(model_path);
        unlink(output_path);
        rmdir(inputs_dir);
        rmdir(job_dir);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: keeping scratch dir '%s'\n", job_dir);
    }
    return err;
}

/* ---- NeutronBackendOps glue ---- */

static void neutron_backend_runner_run(void *opaque,
                                       const NeutronComputeReq *req,
                                       NeutronComputeRes *res)
{
    RunnerCtx *ctx = opaque;
    const RunnerFixture *fx;

    res->retcode        = NEUTRON_RUNNER_DONE;
    res->guest_errcode  = 0;
    res->output_written = false;

    /* Non-RUN commands (KERNELS etc.) are ACKed without spawning anything. */
    if (req->cmd != 0x269 /* N_CMD_RUN */) {
        return;
    }

    fx = runner_match(ctx, req);
    if (!fx) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "neutron-runner: no fixture for (microcode=0x%x, "
                      "tensor_count=%u); reporting fixture-miss\n",
                      req->microcode_offset, req->tensor_count);
        res->guest_errcode = NEUTRON_RUNNER_ERR_FIXTURE_MISS;
        return;
    }

    uint32_t err = runner_run_one(ctx, fx, req);
    if (err) {
        res->guest_errcode = err;
        return;
    }
    res->output_written = true;
}

const NeutronBackendOps neutron_backend_runner = {
    .name = "runner",
    .run  = neutron_backend_runner_run,
};

/* ---- lifecycle ---- */

void *neutron_backend_runner_new(const NeutronRunnerCfg *cfg, Error **errp)
{
    RunnerCtx *ctx;

    if (!cfg->runner_path || !*cfg->runner_path) {
        error_setg(errp, "neutron-runner: neutron-runner-path is required");
        return NULL;
    }
    if (!cfg->fixtures_path || !*cfg->fixtures_path) {
        error_setg(errp, "neutron-runner: neutron-runner-fixtures is required");
        return NULL;
    }
    if (!g_file_test(cfg->runner_path, G_FILE_TEST_IS_EXECUTABLE)) {
        error_setg(errp, "neutron-runner: '%s' is not an executable file",
                   cfg->runner_path);
        return NULL;
    }

    ctx = g_new0(RunnerCtx, 1);
    ctx->runner_path   = g_strdup(cfg->runner_path);
    ctx->fixtures_path = g_strdup(cfg->fixtures_path);
    ctx->scratch_dir   = g_strdup(cfg->scratch_dir ? cfg->scratch_dir : "");
    ctx->timeout_ms    = cfg->timeout_ms ? cfg->timeout_ms
                                         : NEUTRON_RUNNER_DEFAULT_TIMEOUT_MS;
    ctx->keep_scratch  = cfg->keep_scratch;

    if (!runner_load_fixtures(ctx, errp)) {
        neutron_backend_runner_free(ctx);
        return NULL;
    }

    return ctx;
}

/*
 * Register the runner-backend QOM properties on the front end's DeviceClass.
 * We build a runtime Property[] with offsets supplied by the front end (see
 * the header for the rationale). This is why the string "neutron-runner-*"
 * only ever appears in a Track A file (this one and the stub) - the front end
 * literally does not contain the property-name strings, which is what the
 * hygiene test enforces.
 */
void neutron_backend_runner_add_props(DeviceClass *dc,
                                      const NeutronRunnerPropOffsets *o)
{
    static const Property props_tmpl[] = {
        { .name = "neutron-runner-path",        .info = &qdev_prop_string },
        { .name = "neutron-runner-fixtures",    .info = &qdev_prop_string },
        { .name = "neutron-runner-scratch-dir", .info = &qdev_prop_string },
        { .name = "neutron-runner-timeout-ms",  .info = &qdev_prop_uint32,
          .set_default = true, .defval.u = 10000 },
        { .name = "neutron-keep-scratch",       .info = &qdev_prop_bool,
          .set_default = true, .defval.u = false },
    };
    /*
     * QOM keeps the Property pointer we hand to device_class_set_props_n()
     * alive for the lifetime of the class (both dc->props_ and each
     * ObjectProperty's opaque back-reference are captured, not copied). The
     * array must therefore have process lifetime. A stack local here is a
     * use-after-scope trap: it manifests as a SIGSEGV at the first device
     * realize (e.g. qtest_initf running machine init). Allocate on the heap
     * once and never free - one class_init call per program, matches the
     * lifetime of the DeviceClass itself.
     */
    Property *props = g_new(Property, ARRAY_SIZE(props_tmpl));

    memcpy(props, props_tmpl, sizeof(props_tmpl));
    props[0].offset = o->runner_path_off;
    props[1].offset = o->fixtures_off;
    props[2].offset = o->scratch_dir_off;
    props[3].offset = o->timeout_ms_off;
    props[4].offset = o->keep_scratch_off;

    device_class_set_props_n(dc, props, ARRAY_SIZE(props_tmpl));
}

bool neutron_backend_runner_is_stub(void)
{
    return false;
}

void neutron_backend_runner_free(void *opaque)
{
    RunnerCtx *ctx = opaque;
    if (!ctx) {
        return;
    }
    for (size_t i = 0; i < ctx->num_fixtures; i++) {
        g_free(ctx->fixtures[i].name);
        g_free(ctx->fixtures[i].tflite_path);
    }
    g_free(ctx->fixtures);
    g_free(ctx->runner_path);
    g_free(ctx->fixtures_path);
    g_free(ctx->scratch_dir);
    g_free(ctx);
}
