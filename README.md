# HellCore
Source code for the HellCore project, a fork of LambdaMOO.

## Dependencies:

* autoconf
* automake
* bison
* gcc
* gperf
* libcrypt (any compliant implementation, libxcrypt is a good option if your
    glibc no longer supplies a libcrypt and you're on a distro/in an environment
    that for some reason hasn't already added a replacement)
* libstdc++
* make
* sed

If you are unable to build despite having working and recent versions of these
installed, please file a bug report on this repository.

## README

This is an attempt at making a hellcore MOO database. Started from HellMOO, I
deleted tons of Hell-specific objects and verbs. I might have deleted useful
stuff, I more than likely left references to useless/deleted props/objects all
over the place.

Before you start the server, read the section on building it.

To start it, you can run
```shell
./restart hellcore
```

You can also start it with something like this:
```shell
./moo -l moo.log hellcore.db hell.db.test 7777
```

```
(./moo -l <logfile> <original DB> <new DB> <port>)
```

You should be able to login with 'connect Wizard', then change your password.

### Building

Simply run
```shell
./build.sh
```

The moo binary will be located as `src/moo`.

If you are familiar with the standard Linux/Unix build process, you can instead
run
```shell
./autogen.sh && ./configure && make
```

### Troubleshooting

If you're having trouble building after updating this repository, parts of the
build system may be out of date.

Before trying to build again, try running
```shell
./autogen.sh && make distclean
```

### Optional Features

A few features can optionally be enabled/disabled when compiling.

If you use `./build.sh`, you should get a reasonable set of defaults.

#### More Secure Password Hashes

If you have a more modern libcrypt implementation, such as libxcrypt, and it
provides `crypt_gensalt`, the moo will use this to generate more secure and
modern password hashes.

This means that if your database uses the `crypt()` builtin to store passwords,
it will be able to use a format which is far more secure, as the old password
format can be be cracked fairly quickly on modern computers if someone can
manage to get a copy of your database.

This should generally be fairly backwards-compatible with older databases, and
old password hashes will still be read fine after this change, but new passwords
hashed will be more secure.

The new password hashes won't be properly understood by older versions of the
`crypt()` builtin, and may not be understood by versions of the moo built
without this feature, however, so if you want to disable the more secure hashes
to ensure that your database can be used with older versions of the moo binary,
you can pass `--disable-crypt-gensalt` to `./configure` when building.

Note that if your distro has built your libcrypt provider without support for
old, insecure hashes, regardless of whether you have this feature enabled or
not, you may need to, e.g., install via your package manager a build of
libxcrypt which has support for insecure hashes enabled in order to authenticate
against password hashes from older databases.

### USE AT YOUR OWN RISK. I DENY RESPONSIBILITY FOR:
* Spontaneous hairy nose
* Micropenis
* Liver vs pancreas internal war
* Making baby Jesus retch
* Getting on the No-Fly list
* Lizard people living under the Appalachian Mountains following you around

Cheers,

Senso/Dionysus, Necanthrope, diatomic.ge
