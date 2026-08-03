// Defining TU for link-reimport-e2e.c: an array and an extern POINTER
// global initialized into it -- SPIKE 2's fact-starvation shape. A solo
// import of THIS TU emits only the array (the cursor's index global is
// materialized lazily, on use, and nothing here uses it), and a solo
// import of the accessing TU cannot type the cursor at all: only a joint
// re-import of the two can.

int arr[4] = {1, 2, 3, 4};
int *cursor = arr;
