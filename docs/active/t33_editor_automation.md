# T33 Editor and Automation Lifecycle

The user main Editor is the host for ordinary UAI queries, mutations, readback, screenshots, and service checks. Those operations do not own that process and must leave it running.

Plugin or Editor C++ builds use `scripts/ue/automation/Invoke-UePluginBuildLifecycle.ps1`. If the project main Editor was running, the entry requests a normal window close, refuses to force-kill a non-closing Editor, performs the ordinary Development build with Live Coding disabled, and restores the main Editor minimized with UAI auto-start. If the Editor was already closed, the build does not start one.

Automation never targets the main Editor. `TryResolveAutomationHostContract` selects an owned NullRHI worker for non-rendering tests and an owned RHI worker for rendering tests. Slate or foreground interaction requires a separate owned interactive runner; while that runner is unavailable the request fails as `interactive_requires_owned_interactive_runner`.

Every owned worker has its own process identity, `UserDir`, UAI port, fault port, credential configuration, and cleanup proof. The runner does not force foreground activation, disable background throttling, or change global FPS controls. A 10 FPS gate is decided only from the worker's observed runtime receipt. Low or missing applicable evidence blocks the test instead of being hidden.
