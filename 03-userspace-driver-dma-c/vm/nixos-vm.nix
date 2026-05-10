{ pkgs ? import <nixpkgs> {} }:

let
  nixos = import (pkgs.path + "/nixos") {
    configuration = { config, pkgs, lib, ... }: {
      # Kernel + headers for module development
      boot.kernelPackages = pkgs.linuxPackages_latest;

      # Build tools
      environment.systemPackages = with pkgs; [
        gcc
        gnumake
        pkg-config
      ];

      # Auto-login as root (no password prompt)
      services.getty.autologinUser = "root";

      # Mount host filesystem via 9p
      fileSystems."/mnt" = {
        device = "hostfs";
        fsType = "9p";
        options = [ "trans=virtio" "version=9p2000.L" "msize=104857600" ];
        neededForBoot = false;
      };

      # Minimal VM — no GUI, no network needed
      services.xserver.enable = false;
      networking.hostName = "nic-dev";

      # QEMU guest agent
      services.qemuGuest.enable = true;

      system.stateVersion = "24.11";
    };
  };
in
  nixos.vm
