#!/bin/bash -x
#pushd /mnt/hdd2/acyriac/Workspace/cel_ww48/out/target/product/cel_apl/obj/kernel/
pushd /mnt/hdd4/amritara/Workspace/cel_april2/out/target/product/cel_kbl/obj/kernel/
./scripts/sign-file sha256 certs/signing_key.pem certs/signing_key.x509 /mnt/hdd4/amritara/Workspace/cel_april2/kernel/btusb_sco/btusb_sco_snd_card.ko
popd 
