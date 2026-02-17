/* Stub replacements for gwbench.c functions.
 * The real gwbench.c includes sqlite3.c (8MB amalgamation) which conflicts
 * with Bitcoin Core's own SQLite. We don't need benchmark database lookups
 * for Freycoin's use of gwnum (just FFT modular arithmetic for nextprime).
 * These stubs return "no data" so gwnum falls back to its built-in defaults. */

void gwbench_read_data(int x) {
    (void)x;
    /* No benchmark database to read. */
}

int internal_implementation_ids_match(int a, int b, int c, int d, int e, int f, int g, int h) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    return 0; /* Never match — forces gwnum to use its default implementation selection. */
}

void gwbench_get_max_throughput(int a, int b, int c, int d, int e, int f, int g, int *out_impl, double *out_throughput) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    if (out_impl) *out_impl = 0;
    if (out_throughput) *out_throughput = 0.0;
    /* No benchmark data — gwnum will use its default FFT implementation. */
}
