// fixtures.js — the host list and terminal text the sketches draw with.
// The lab keeps the same data (PROFILES in sim/uilab.c, term_fill).

export const PROFILES = [
  { name: 'homelab', where: 'root@192.168.1.10:22' },
  { name: 'build-box', where: 'ci@build.lan:22  key' },
  { name: 'pi-garage', where: 'pi@10.0.0.42:22' },
  { name: 'vps-fra', where: 'deploy@fra1.example.net:2222  key' },
  { name: 'router', where: 'admin@192.168.1.1:22' },
  { name: 'nas', where: 'backup@nas.lan:22  key' },
  { name: 'printer', where: 'octo@10.0.0.77:22' },
  { name: 'bastion', where: 'ops@bastion.example.org:22  key' },
];

// The same list grown to 32 hosts, for the paged HOME frames.
export const PROFILES32 = [
  ...PROFILES,
  { name: 'db-primary', where: 'postgres@db1.lan:22  key' },
  { name: 'db-replica', where: 'postgres@db2.lan:22  key' },
  { name: 'git', where: 'git@git.example.org:2222  key' },
  { name: 'mail', where: 'admin@mail.example.net:22' },
  { name: 'dns-a', where: 'root@10.0.0.2:22' },
  { name: 'dns-b', where: 'root@10.0.0.3:22' },
  { name: 'proxmox', where: 'root@pve.lan:22' },
  { name: 'k3s-node1', where: 'ubuntu@10.0.1.11:22  key' },
  { name: 'k3s-node2', where: 'ubuntu@10.0.1.12:22  key' },
  { name: 'k3s-node3', where: 'ubuntu@10.0.1.13:22  key' },
  { name: 'media', where: 'plex@media.lan:22' },
  { name: 'backup', where: 'borg@backup.lan:2200  key' },
  { name: 'vps-ams', where: 'deploy@ams1.example.net:2222  key' },
  { name: 'vps-nyc', where: 'deploy@nyc1.example.net:2222  key' },
  { name: 'lab-gpu', where: 'ml@gpu-01.lab:22  key' },
  { name: 'lab-arm', where: 'pi@arm-01.lab:22' },
  { name: 'iot-hub', where: 'pi@10.0.2.5:22' },
  { name: 'cam-gate', where: 'cam@10.0.2.31:2222' },
  { name: 'weather', where: 'pi@10.0.2.40:22' },
  { name: 'ci-runner', where: 'runner@ci2.lan:22  key' },
  { name: 'staging', where: 'deploy@stg.example.com:22  key' },
  { name: 'prod-web', where: 'deploy@web1.example.com:22  key' },
  { name: 'prod-api', where: 'deploy@api1.example.com:22  key' },
  { name: 'office-fw', where: 'admin@fw.office.example.com:8022' },
];

export const TERM_LINES = [
  'deck@homelab:~$ journalctl -fu sshd',
  'Nov 12 14:02:11 homelab sshd[811]: Accepted publickey for deck',
  'Nov 12 14:02:11 homelab sshd[811]: pam_unix(sshd:session): opened',
  'Nov 12 14:03:40 homelab systemd[1]: Started Daily apt upgrade.',
  'Nov 12 14:04:02 homelab kernel: wlan0: associated',
  'deck@homelab:~$ htop',
  '  PID USER      PRI  NI  VIRT   RES   SHR S CPU% MEM%   TIME+',
  '  811 deck       20   0 16.2M 6304K 5120K S  0.7  0.3  0:00.42',
  '    1 root       20   0  167M 11.9M 8448K S  0.0  0.6  0:02.10',
];
