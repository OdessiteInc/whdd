#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "libdevcheck.h"
#include "device.h"
#include "action.h"

static int action_find_start_perform_until_interrupt(DC_Dev *dev, char *act_name,
                ActionDetachedLoopCB callback, void *callback_priv);
static int readtest_cb(DC_ActionCtx *ctx, void *callback_priv);
static int zerofill_cb(DC_ActionCtx *ctx, void *callback_priv);
static void show_legend(void);

DC_Ctx *dc_ctx;

int main() {
    int r;
    // init libdevcheck
    dc_ctx = dc_init();
    assert(dc_ctx);
    // get list of devices
    DC_DevList *devlist = dc_dev_list(dc_ctx);
    assert(devlist);
    // show list of devices
    int devs_num = dc_dev_list_size(devlist);
    if (devs_num == 0) { printf("No devices found\n"); return 0; }

    while (1) {
    // print actions list
    printf("\nChoose action #:\n"
            "0) Exit\n"
            "1) Show SMART attributes\n"
            "2) Perform read test\n"
            "3) Perform 'write zeros' test\n"
          );
    int chosen_action_ind;
    r = scanf("%d", &chosen_action_ind);
    if (r != 1) {
        printf("Wrong input for action index\n");
        return 1;
    }
    if (chosen_action_ind == 0) {
        printf("Exiting due to chosen action\n");
        return 0;
    }

    int i;
    for (i = 0; i < devs_num; i++) {
        DC_Dev *dev = dc_dev_list_get_entry(devlist, i);
        printf(
                "#%d:" // index
                " %s" // /dev/name
                " %s" // model name
                // TODO human-readable size
                " %"PRIu64" bytes" // size
                "\n"
                ,i
                ,dev->dev_fs_name
                ,dev->model_str
                ,dev->capacity
              );
    }
    printf("Choose device by #:\n");
    int chosen_dev_ind;
    r = scanf("%d", &chosen_dev_ind);
    if (r != 1) {
        printf("Wrong input for device index\n");
        return 1;
    }
    DC_Dev *chosen_dev = dc_dev_list_get_entry(devlist, chosen_dev_ind);
    if (!chosen_dev) {
        printf("No device with index %d\n", chosen_dev_ind);
        return 1;
    }

    switch (chosen_action_ind) {
    case 1:
        ;
        char *text;
        text = dc_dev_smartctl_text(chosen_dev, "-A -i");
        if (text)
            printf("%s\n", text);
        free(text);
        break;
    case 2:
        show_legend();
        action_find_start_perform_until_interrupt(chosen_dev, "readtest", readtest_cb, NULL);
        break;
    case 3:
        printf("This will destroy all data on device %s (%s). Are you sure? (y/n)\n",
                chosen_dev->dev_fs_name, chosen_dev->model_str);
        char ans = 'n';
        r = scanf("\n%c", &ans);
        if (ans != 'y')
            break;
        show_legend();
        action_find_start_perform_until_interrupt(chosen_dev, "zerofill", zerofill_cb, NULL);
        break;
    default:
        printf("Wrong action index\n");
        break;
    }
    } // while(1)

    return 0;
}

struct block_speed_vis {
    uint64_t access_time; // in mcs
    char vis; // visual representation
};

struct block_speed_vis bs_vis[] = {
    { 1000, '`' },
    { 2000, '.' },
    { 5000, ':' },
    { 10000, '=' },
};
char exceed_vis = '#';
char error_vis = '!';

char choose_vis(uint64_t access_time) {
    int i;
    for (i = 0; i < sizeof(bs_vis)/sizeof(*bs_vis); i++)
        if (access_time <= bs_vis[i].access_time)
            return bs_vis[i].vis;
    return exceed_vis;
}

static void show_legend(void) {
    int i;
    for (i = 0; i < sizeof(bs_vis)/sizeof(*bs_vis); i++)
        printf(" -- %c -- access time <= %"PRIu64" microseconds\n", bs_vis[i].vis, bs_vis[i].access_time);
    printf(" -- %c -- access time exceeds any of above\n", exceed_vis);
    printf(" -- %c -- access error\n", error_vis);
}

static int readtest_cb(DC_ActionCtx *ctx, void *callback_priv) {
    if (ctx->performs_executed == 1) {
        printf("Performing read-test of '%s' with block size of %"PRIu64" bytes\n",
                ctx->dev->dev_fs_name, ctx->blk_size);
    }
    if (ctx->report.blk_access_errno)
        putchar(error_vis);
    else
        putchar(choose_vis(ctx->report.blk_access_time));
    fflush(stdout);
    return 0;
}

static int zerofill_cb(DC_ActionCtx *ctx, void *callback_priv) {
    if (ctx->performs_executed == 0) {
        printf("Performing 'write zeros' test of '%s' with block size of %"PRIu64" bytes\n",
                ctx->dev->dev_fs_name, ctx->blk_size);
    }
    if (ctx->report.blk_access_errno)
        putchar(error_vis);
    else
        putchar(choose_vis(ctx->report.blk_access_time));
    fflush(stdout);
    return 0;
}

static int action_find_start_perform_until_interrupt(DC_Dev *dev, char *act_name,
        ActionDetachedLoopCB callback, void *callback_priv
        ) {
    int r;
    int sig;
    sigset_t set;
    pthread_t tid;
    DC_Action *act = dc_find_action(dc_ctx, act_name);
    assert(act);
    DC_ActionCtx *actctx;
    r = dc_action_open(act, dev, &actctx);
    if (r) {
        printf("Action init fail\n");
        return 1;
    }

    sigemptyset(&set);
    sigaddset(&set, SIGQUIT);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGTERM);
    r = pthread_sigmask(SIG_BLOCK, &set, NULL); /* new created threads will inherit this set */
    if (r) {
        printf("p_sigmask failed: %d\n", r);
        goto fail;
    }

    r = dc_action_perform_loop_detached(actctx, callback, callback_priv, &tid);
    if (r) {
        printf("dc_action_perform_loop_detached fail\n");
        goto fail;
    }

    r = sigwait(&set, &sig);
    assert(!r);

    printf("got signal %d, interrupting action %s\n", sig, act_name);
    actctx->interrupt = 1;
    printf("waiting for detached action loop to join\n");
    r = pthread_join(tid, NULL);
    assert(!r);

    dc_action_close(actctx);
    return 0;

fail:
    dc_action_close(actctx);
    return 1;
}
