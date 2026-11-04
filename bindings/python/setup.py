# CPython extension build: compiles runtime/pbwire.c together with the C core
# (src/*.c), so a wheel is self-contained -- no libpbwire to install.
# The version is the repo-root VERSION file: never hardcode it here.
import os
import shutil
from setuptools import Extension, setup
from setuptools.command.sdist import sdist as _sdist

HERE = os.path.dirname(os.path.abspath(__file__))


def find_root():
    # In a repo checkout the core lives two levels up; inside an sdist the
    # sdist command stages copies of it next to setup.py (see below).
    d = HERE
    for _ in range(4):
        if os.path.isfile(os.path.join(d, "VERSION")) and os.path.isdir(
            os.path.join(d, "src")
        ):
            return d
        d = os.path.dirname(d)
    return None


ROOT = find_root()

CORE = [
    "wire.c", "arena.c", "schema.c", "message.c", "encode.c", "decode.c",
    "ctx.c", "descriptor_set.c", "text.c", "json.c", "stream.c",
]

if ROOT is None:
    # Building from an sdist: staged copies.
    with open(os.path.join(HERE, "VERSION"), encoding="utf-8") as f:
        VERSION = f.read().strip()
    SOURCES = [os.path.join("runtime", "pbwire.c")] + [
        os.path.join("core", c) for c in CORE
    ]
    INCLUDE = [os.path.join(HERE, "include")]
else:
    with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as f:
        VERSION = f.read().strip()
    SOURCES = [os.path.join("runtime", "pbwire.c")] + [
        os.path.join(ROOT, "src", c) for c in CORE
    ]
    INCLUDE = [os.path.join(ROOT, "include")]


class sdist(_sdist):
    # A monorepo sdist: stage the core sources, headers and VERSION next to
    # setup.py so the isolated wheel build (which only sees this directory)
    # stays self-contained.
    def make_release_tree(self, base_dir, files):
        super().make_release_tree(base_dir, files)
        if ROOT is None:
            return
        shutil.copytree(
            os.path.join(ROOT, "src"), os.path.join(base_dir, "core")
        )
        # The optional C++ importer is not part of the wheel.
        shutil.rmtree(os.path.join(base_dir, "core", "protoc"), ignore_errors=True)
        shutil.copytree(
            os.path.join(ROOT, "include"), os.path.join(base_dir, "include")
        )
        shutil.copy2(
            os.path.join(ROOT, "VERSION"), os.path.join(base_dir, "VERSION")
        )


setup(
    name="pbwire",
    version=VERSION,
    description="Dynamic protobuf over the pbwire C ABI, schema at runtime, no generated code",
    license="Apache-2.0",
    author="jinganix",
    url="https://github.com/jinganix/pbwire",
    python_requires=">=3.9",
    ext_modules=[
        Extension("pbwire", sources=SOURCES, include_dirs=INCLUDE)
    ],
    cmdclass={"sdist": sdist},
)
