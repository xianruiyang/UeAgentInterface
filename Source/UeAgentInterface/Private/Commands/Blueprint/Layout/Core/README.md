# Core

This stage is the pure Blueprint auto layout algorithm.

It accepts `FNode`, `FEdge`, and `FOptions`, then produces updated node
positions, route plans, metrics, and diagnostics. It must not read or mutate UE
assets, query Slate, inspect graph widgets, save packages, or parse command
parameters.
