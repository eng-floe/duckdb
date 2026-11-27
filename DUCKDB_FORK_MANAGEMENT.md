# DuckDB Fork Management

## Overview

We are open-sourcing a fork of DuckDB with the primary objective to release Floe V1 as quickly as possible. This fork will facilitate our own product development and contribution to upstream.

We will actively contribute to the open-source community as time allows—cherry-picking and submitting as many of our fixes upstream as possible. 

Please read the DuckDB [release cycle](https://duckdb.org/docs/preview/dev/release_cycle) and [release calendar](https://duckdb.org/release_calendar) documentation as a reference. Note that 2.0 (which is set to be released on the 21st October 2026\) also introduces a [new stable C API](https://github.com/duckdb/duckdb/blob/v2.0-cyanoptera/api_spec/README.md), [from which all other client APIs](https://duckdb.org/2026/08/17/duckdb-20-highlights#developing-extensions) (including C++) will be based; 2.0 is the version that we will initially base ourselves on and also happens to be an LTS release (and so is supported for a year). Currently, most popular extensions are built on top of the C++ API (which is basically anything in DuckDB that you can link to) which in versions before 2.0 is considered unstable.

## Development

### Keeping track of upstream

We will create a public fork (this gives us free CI\!) of DuckDB ensuring that initially we bring over their main branch, v2.0-cyanoptera and will also fetch their v.2.0.0 tag when it is created. We will create automated jobs that keep the main branch and the current LTS branch that we are on (e.g., v2.0-cynaoptera) up-to-date. Likewise, we will fetch new patch tags (e.g., 2.0.1) for the LTS version that we are on as and when they are created. As a rule, we never add our own commits to any upstream branch to not interfere with any automation we build around this. When an LTS version reaches EOL, we no longer need to poll the corresponding branch and tags. Note, we do not run CI on any upstream branches \- we expect DuckDB to be doing its job here.

For every tag that we bring in, we also create a corresponding branch suffixed with “-floe”. For example, if we start tracking v2.0.0, we create a branch named v2.0.0-floe. Here, we can place any custom commits that we might require; how and under what conditions we add custom commits will be elaborated on later in the document. When a new patch release is created upstream such as 2.0.1, we create a branch locally named v2.0.1-floe based off of the v2.0.0-floe branch. In this branch, we rebase onto the v2.0.1 tag, which should be a clean replay of just our custom commits and push it. When a new LTS version is released such as 2.2, we create a branch named v2.2.0-floe that is based off of the v2.2.0 tag and cherry-pick relevant commits on top of it from the existing v2.x.y-floe branch. At any one time there is only one active Floe branch (i.e., suffixed with \-floe) and can be determined by the “newest’ semver branch suffixed with “-floe. This active branch must have CI enabled and working.

At any point in time, the active branch must be based on a supported LTS DuckDB version. Being based on an LTS version means we get bugfixes for a year. LTS releases are released approximately every 8 months so this gives us around 4 months to move between supported LTS releases, which should be plenty of time to apply any custom patches we may have; some patches may even be dropped if they are upstreamed in time.

### Keeping track of our fork

In the core repository, we will bring in our fork of DuckDB as a Git submodule. At all times, we only use a SHA that is the (current) tip of an appropriate branch suffixed with (-floe). This SHA is only updated when there is a change in the corresponding Floe branch (i.e., we have committed an additional patch ) or the active Floe branch has changed (we have integrated an upstream DuckDB release). The bump of the SHA must be atomic, in the sense that the core repository must build and run tests successfully with this bump.

### Custom Patches

Before the upstreaming process is codified, the following guiding principle must be stated: where feasible avoid making changes to DuckDB and instead attempt to implement relevant functionality using the [stable C API](https://github.com/duckdb/duckdb/blob/v2.0-cyanoptera/api_spec/README.md) (or APIs, such as the C++ API that are based on this stable C API); if that is not feasible changes should be made in such a way that they are (potentially) upstreamable and/or expose relevant functionality as extensions to avoid the custom patch in the first place.

However, there will be a need to create custom commits and we need to establish a process on how we attempt to upstream these patches where feasible, depending on the type of change being made. In any case, all changes should follow their [contribution guidelines](https://github.com/duckdb/duckdb/blob/main/CONTRIBUTING.md), [code of conduct](https://github.com/duckdb/duckdb/blob/main/CODE_OF_CONDUCT.md) and [AI policy](https://github.com/duckdb/duckdb/blob/main/AI_POLICY.md).

Most of our code should go into new files not existing ones.
Use single line hooks with names prefixed floe_ to invoke them.
Subclass and override rather than hack base classes.
Use identifiable floe comments blocks to bound code changes that can’t fit the above. 


#### Patching Process

First, create a branch off of the active Floe branch and commit your custom patch there. Open a PR (ensure CI runs and passes) onto the active Floe branch and get your PR reviewed internally; the reviewer should attempt to emulate a DuckDB reviewer, ensuring that contribution guidelines are being met. All such PRs must be fast-forward merged to create a linear history that is easy to rebase in the future. Only once it is reviewed and merged internally can you open a PR to the upstream DuckDB project.

If you believe the patch to be upstreamable, read the documentation regarding the upstream [release cycle](http://duckdb.org/docs/preview/dev/release_cycle). In particular, keep in mind what phase of the release cycle the upstream project is in and determine if your change will be considered a bug-fix or a feature. Based on reading the documentation, you should have an understanding of what branch you are targeting (e.g.,v2.0-cynaoptera). Create a branch off of the target (upstream) branch and cherry-pick your change from the active Floe branch onto this new branch. Create a PR in the upstream project to the relevant target branch with the source being your new branch. If it is merged, you should delete the corresponding branch you created for the PR.

### Extensions

Any extensions that we write will be part of the core repository. We can use the [extension-template](https://github.com/duckdb/extension-template) as an inspiration about how to structure our extension(s); note that some parts are not particularly relevant to us (like re-using their CI tools, as we’re not planning to publish our extension). Where feasible, we should target only depending on the [stable C API](https://github.com/duckdb/duckdb/blob/v2.0-cyanoptera/api_spec/README.md) (or any API based on it, like their C++ API) to make bumping the DuckDB submodule trivial.
