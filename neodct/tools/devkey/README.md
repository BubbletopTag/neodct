# Development signing key

`neodct-dev.key` is a **local development** private key. It is ignored by git
(`.gitignore` covers `*.key`), so it exists only on this machine and a fresh
clone will not have one.

Its public half is committed as
`neodct/overlay/NeoDCT/System/keys/neodct-release.pub`, which is the key the
phone checks update signatures against.

## Signing a build

    NEODCT_SIGN_KEY=$PWD/neodct/tools/devkey/neodct-dev.key \
        make -C buildroot update

Without `NEODCT_SIGN_KEY` the package is still built, just unsigned -- the
phone then shows "BAD SIGNATURE! UPDATE MAY BE CORRUPT!!", which engineering
mode can override. That is a useful path to test on purpose.

## Before shipping to anyone else

Generate a real release key, keep the private half offline, and replace the
committed public key with its public half:

    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:4096 \
        -out neodct-release.key
    openssl rsa -in neodct-release.key -pubout \
        -out neodct/overlay/NeoDCT/System/keys/neodct-release.pub

Anyone holding the private key that matches the public key in an image can
sign an update that phone will install without complaint. While the dev key
is the one in the image, treat "signed" as meaning nothing more than "built
on this machine".
