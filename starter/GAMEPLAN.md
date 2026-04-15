# Assignment 3 Game Plan (Execution Checklist)

## What the assignment requires
1. Start from Dan Luu's malloc tutorial implementation (`malloc`, `free`, `calloc`, `realloc`).
2. Add a driver `main()` with **at least 10 calls each** to `malloc/calloc/realloc/free`.
3. Print heap start/end addresses and memory leak measurements.
4. Convert SLL allocator metadata/list into a DLL version.
5. Implement **best-fit** and **worst-fit** free-block selection and compare leaks vs first-fit.
6. Implement block **splitting**.
7. Implement adjacent free-block **coalescing**.
8. Implement a **two-thread** driver with `pthread_create`, and reason about per-thread heaps/lists.
9. Turn in **three `.c` files** (SLL+first-fit, DLL+best-fit, DLL+worst-fit) plus report.

## Suggested implementation order
1. Finish `starter/malloc_firstfit_sll.c` until stable.
2. Add robust leak accounting + randomized stress harness.
3. Fork first-fit file into DLL layout and implement best-fit.
4. Fork best-fit into worst-fit by only swapping selection policy.
5. Add splitting, then coalescing, then regression tests.
6. Add multithreaded driver scenario and thread-local/per-thread bookkeeping.
7. Fill `starter/report_template.md` with data from repeatable runs.

## Minimum deliverables checklist
- [ ] `malloc_firstfit_sll.c` complete and tested.
- [ ] `malloc_bestfit_dll.c` complete and tested.
- [ ] `malloc_worstfit_dll.c` complete and tested.
- [ ] Main driver includes >=10 calls each allocator API.
- [ ] Leak table across multiple run sizes.
- [ ] Discussion of best/worst/first behavior.
- [ ] Two-thread experiment + explanation.
- [ ] Final report PDF.
