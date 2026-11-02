# Security

The runtime and the tools run native code on your PC and read files you give them: game dumps, NSO modules, save data, shader and texture caches. A bug in how they parse those files, or in how they handle memory, can be a security problem.

If you find one, report it privately through the repository's Security tab ("Report a vulnerability"). Do not open a public issue. Please include the version or commit, what you did, and what happened. We will reply, work on a fix, and credit you when it ships if you want that.

Things we treat as security problems:

- a crafted NSO, NSZ, ROM or save file that crashes or runs code in the loader, the runtime or the setup tool
- the setup tool writing outside the folder you chose
- the recompiler or a tool executing something from an input file

Everything else (crashes with normal files, graphics bugs, wrong behaviour) goes through the normal issue forms.

Only the current `main` branch gets fixes.
