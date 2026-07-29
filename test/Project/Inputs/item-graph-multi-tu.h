// Shared header of the FR-40 multi-TU item-graph test: one external
// prototype seen by BOTH translation units, so the graph's cross-TU
// unification of external symbols is exercised.
#ifndef EMITRUST_TEST_ITEM_GRAPH_MULTI_TU_H
#define EMITRUST_TEST_ITEM_GRAPH_MULTI_TU_H

int shared_step(void);

#endif // EMITRUST_TEST_ITEM_GRAPH_MULTI_TU_H
