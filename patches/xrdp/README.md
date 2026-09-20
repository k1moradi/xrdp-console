# xrdp patch series

The optimized xrdp checkout is still kept under
`third_party/xrdp-0.10.6.1-optimized` during the repository migration. Its
required local changes have not yet been split into this directory.

The dependency-migration step will pin upstream xrdp 0.10.6.1, fetch it under
`build/_deps/`, and replace the checked-in fork with small, reviewable patches
here. Do not add new first-party runtime code to the upstream tree.
