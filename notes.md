Added
- The serial log opens with the NeoDCT name and version, and every service
  now has its own colour. The modem is blue, the system green, anything
  that failed is red.
- The modem talks to the serial console as it works. It used to keep
  everything it knew in a file on the phone.

Fixed
- Mobile data. Each dial attempt was taking a connection slot from the
  modem and never giving it back, so after enough tries the modem stopped
  answering until it was power-cycled.


---

**Installing:** the phone finds this by itself — *Update* → *Look online*.
It downloads the package for its own platform onto the SD card and
installs it. Or copy the `.ndsw` into the `update` folder on the card by
hand.

**About the signature.** These packages are signed with the project's
development key, and the matching public key is built into the images
published here. That means:

- On a phone running an image from this repository, the signature verifies
  and the update installs normally.
- On a phone built from your own tree, it will **not** verify. You will see
  `BAD SIGNATURE! UPDATE MAY BE CORRUPT!!`. That is the phone doing its
  job, not a bug — anyone holding a key matching the one in your image can
  replace your entire root filesystem. Build and sign your own packages
  with your own key (see `neodct/tools/devkey/README.md`).
