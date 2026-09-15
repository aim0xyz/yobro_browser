# Third-party evidence for the Chromium feasibility artifact

This directory accompanies the local **YOBRO Chromium Feasibility** package. It
is an engineering artifact, not a production distribution or a legal approval.

The packaging step adds the following files from the exact Qt/Homebrew inputs
used to build the artifact:

- Chromium's top-level `LICENSE.Chromium` notice;
- SPDX inventories selected as packaging evidence for Qt Base, Declarative,
  Image Formats, PDF, Positioning, Serial Port, SVG, Virtual Keyboard,
  WebChannel, and WebEngine; the local minimized artifact deploys only the
  runtime subset reported by the recursive package validator;
- Homebrew SPDX SBOMs for the Qt and Qt WebEngine 6.11.2 packages.

Qt may be used under commercial or open-source terms depending on the project's
license entitlement and distribution model. Before any public release, the
selected license route, complete third-party notices, corresponding-source and
relinking obligations, signing entitlements, and every actually deployed
framework/plugin must be reviewed. The included SPDX inventories are evidence
of the evaluated build inputs; they are not a substitute for that review.
