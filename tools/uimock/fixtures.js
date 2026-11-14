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
