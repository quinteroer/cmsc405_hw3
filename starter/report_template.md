# CMSC 405 Assignment 3 Report Template

## 1) What is working / not working
- **SLL + First Fit:**
- **DLL + Best Fit:**
- **DLL + Worst Fit:**
- **Split blocks:**
- **Coalescing adjacent free blocks:**
- **Two-thread version:**

## 2) Leak measurement method
Explain:
1. how you count allocated-vs-freed bytes,
2. where in `main()` you compute it,
3. what caveats your method has.

## 3) Comparative leak table

| Run ID | #malloc | #calloc | #realloc | #free | First Fit (SLL) | Best Fit (DLL) | Worst Fit (DLL) | Notes |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 1 | 10 | 10 | 10 | 10 |  |  |  | |
| 2 | 100 | 100 | 100 | 100 |  |  |  | |
| 3 | 1000 | 1000 | 1000 | 1000 |  |  |  | |

## 4) Performance behavior discussion
- When best fit is better than first fit:
- When worst fit is better than first fit:
- Cases where they are comparable or worse:
- Root cause trace from allocator internals:

## 5) Multi-thread observations
- Did each thread use separate heaps?
- How did you maintain per-thread lists?
- Any lock contention or race findings?
