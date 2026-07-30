// Companion TU for multi-tu-external-requirement.c: it defines nothing the
// first TU needs, so `host_scale` stays undefined project-wide.
int helper(int v) { return v + 1; }
