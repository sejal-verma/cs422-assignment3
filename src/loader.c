#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdarg.h>

#include <bpf/libbpf.h>

static volatile sig_atomic_t running = 1;

static void stop(int sig)
{
    (void)sig;
    running = 0;
}

static int libbpf_log(enum libbpf_print_level level,
                      const char *format, va_list args)
{
    return vfprintf(stderr, format, args);
}

int main(void)
{
    struct bpf_object *obj = NULL;
    struct bpf_map *map;
    struct bpf_link *link = NULL;
    int err;
    libbpf_set_print(libbpf_log);
    obj = bpf_object__open_file("build/cs422.bpf.o", NULL);
    if (!obj) {
        fprintf(stderr, "Failed to open BPF object\n");
        return 1;
    }

    err = bpf_object__load(obj);
    if (err) {
        fprintf(stderr, "Failed to load BPF object: %d\n", err);
        goto cleanup;
    }

    map = bpf_object__find_map_by_name(obj, "cs422");
    if (!map) {
        fprintf(stderr, "Could not find cs422 struct_ops map\n");
        err = -ENOENT;
        goto cleanup;
    }

    link = bpf_map__attach_struct_ops(map);
    err = libbpf_get_error(link);

    if (err) {
        fprintf(stderr, "Failed to attach struct_ops: %d\n", err);
        link = NULL;
        goto cleanup;
    }

    printf("cs422 registered! Press Ctrl+C to unload.\n");

    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    while (running)
        sleep(1);

cleanup:
    bpf_link__destroy(link);
    bpf_object__close(obj);

    return err ? 1 : 0;
}