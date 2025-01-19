#!/bin/bash

token=
run_args=
make_args="-j $(nproc) pacman-pkg PACMAN_PKGBASE=linux-integration PACMAN_EXTRAPACKAGES="
image=registry.gitlab.steamos.cloud/jupiter/linux-integration/archlinux-kernel-builder:2024-12-02 
file=

usage() {
  echo -e "Usage:"
  echo -e "$0 -h"
  echo -e "$0 [-t <TOKEN>] [<MAKE_ARGS>]"
  echo -e "$0 -f <FILE> [<MAKE_ARGS>]"
  echo -e "$0 -s"
  echo -e "where"
  echo -e "\t-h prints this help message"
  echo -e "\t<TOKEN> is your project/personal access token"
  echo -e "\t<MAKE_ARGS> are your make arguments (targets, -j \$(nproc), ...)"
  echo -e "\t<FILE> use this Dockerfile instead of using latest published image"
  echo -e "\t\tdefault:${make_args}"
  echo -e "\t-s spawns a shell in the docker environment"
}

while getopts ":ht:sf:" opt; do
  case "${opt}" in 
    t)
      token=${OPTARG}
      ;;
    h)
      usage
      exit 0
      ;;
    s)
      run_args="--entrypoint bash"
      make_args=
      ;;
    f)
      file=${OPTARG}
      ;;
    *)
      usage
      exit 1
      ;;
  esac
done

shift $((OPTIND-1))

if [ -n "${token}" ]; then
  echo "logging in to registry..."
  docker login -u $(id -un) -p ${token} registry.gitlab.steamos.cloud
fi

if [ -n "$*" ]; then
  make_args=$*
fi

if [ -n "${file}" ]; then
  image=$(docker build -q -f ${file} $(dirname ${file}))
fi

docker run -it -u $(id -u):$(id -g) -v $(pwd):$(pwd) -w $(pwd) ${image} ${make_args}
if [ "$?" -ne "0" ]; then
  echo "Failed to run docker image. Ensure docker login via"
  echo "docker login -u $(id -un) -p <TOKEN> registry.gitlab.steamos.cloud"
  echo "where <TOKEN> is your project or personal access token for the registry"
  echo "alternatively, use run this script again with '-t <TOKEN>'"
fi

