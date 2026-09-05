#!/bin/sh
# ftp-server-setup.sh -- stand up the file locker the Fetch app downloads from.
#
#   scp neodct/tools/ftp-server-setup.sh root@<droplet>:/root/
#   ssh root@<droplet> 'sh /root/ftp-server-setup.sh'
#
# Run it on the DigitalOcean relay droplet as root, on Debian or Ubuntu. It is
# idempotent: running it again re-reads the config, keeps the certificate and
# only asks for a password if there is not one already.
#
# ============ WHY FTP AT ALL, WITH SSH ALREADY ON THIS BOX ============
#
# The phone has curl and nothing else that speaks a file transfer. curl is in
# both defconfigs for the update system, it does FTP and FTPS out of the box,
# and it costs the image nothing further. sftp:// would need libcurl built
# against libssh2, which is a Buildroot option this tree does not enable, and
# ssh itself authenticates with a key rather than a typed password -- and a
# key on the memory card is a key anybody holding the phone has. A password
# the owner types on the keypad and the phone never stores is the thing that
# actually fits a device you can lose.
#
# ============ EXPLICIT TLS, AND WHAT IT IS AND IS NOT WORTH ============
#
# vsftpd is configured to REQUIRE AUTH TLS on both the control and the data
# connection: the password and the files are encrypted on the wire, and a
# plain-FTP client is refused rather than quietly downgraded. The certificate
# is self-signed, because this droplet has an IP address and no domain name,
# and the phone passes curl -k -- so the encryption is real but the identity
# is not checked, and somebody in a position to redirect traffic to this IP
# could read the password. That was a deliberate choice and it is the reason
# this account should be worth nothing: it holds media, it has no shell
# (/usr/sbin/nologin), and by default it cannot write.
#
# To close that gap later: put a domain on the droplet, run certbot, point
# rsa_cert_file at the LE fullchain, and drop the -k in the app's ftp.c.
#
# ============ THE THREE VSFTPD SETTINGS THAT ARE NOT OPTIONAL ============
#
# Every one of these is a mode where the server looks configured and no
# client can complete a transfer:
#
#   require_ssl_reuse=NO   vsftpd defaults to demanding that the data
#                          connection resume the control connection's TLS
#                          session. curl does not do that, and the transfer
#                          dies after PASV with "550 Permission denied".
#   seccomp_sandbox=NO     vsftpd's sandbox predates modern glibc and its
#                          filter kills the process on syscalls TLS makes,
#                          which surfaces as "500 OOPS: priv_sock_get_cmd".
#   pasv_address=<ip>      the droplet may be behind a NAT-ish setup and
#                          vsftpd would otherwise advertise its private
#                          address in the PASV reply, sending the phone off
#                          to connect to 10.x.
set -eu

FTP_USER="${FTP_USER:-neodct}"
FTP_ROOT="${FTP_ROOT:-/srv/neodct-ftp}"
PASV_MIN="${PASV_MIN:-40000}"
PASV_MAX="${PASV_MAX:-40100}"
# Uploads are off by default: this is a read-only locker the phone pulls from,
# and you already have root over ssh to put files in it. Set WRITABLE=1 if you
# would rather push with an FTP client too.
WRITABLE="${WRITABLE:-0}"

CERT=/etc/ssl/private/vsftpd-neodct.pem
CONF=/etc/vsftpd.conf

[ "$(id -u)" = 0 ] || { echo "run this as root" >&2; exit 1; }

say() { echo "[ftp-setup] $*"; }

# ---- the package ----------------------------------------------------
if ! command -v vsftpd >/dev/null 2>&1; then
    say "installing vsftpd"
    DEBIAN_FRONTEND=noninteractive apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq vsftpd openssl
fi

# ---- the account ----------------------------------------------------
#
# No shell and no home of its own outside the locker: this account exists to
# own files and to be a name in a password prompt, and if it is ever guessed
# the thing it reaches should be the thing it already serves.
if ! id "$FTP_USER" >/dev/null 2>&1; then
    say "creating user $FTP_USER"
    useradd --home-dir "$FTP_ROOT" --shell /usr/sbin/nologin --no-create-home "$FTP_USER"
    say "set the password for $FTP_USER -- this is what you type on the phone"
    passwd "$FTP_USER"
else
    say "user $FTP_USER exists; leaving its password alone (passwd $FTP_USER to change it)"
fi

# ---- the locker -----------------------------------------------------
#
# chroot_local_user makes $FTP_ROOT the whole of the filesystem this account
# can see, and vsftpd REFUSES to chroot into a directory the user can write
# (a writable chroot is an old privilege-escalation route). So the root itself
# stays root-owned and every subdirectory below it is the account's.
say "laying out $FTP_ROOT"
mkdir -p "$FTP_ROOT"
chown root:root "$FTP_ROOT"
chmod 755 "$FTP_ROOT"
for d in music roms naps other; do
    mkdir -p "$FTP_ROOT/$d"
    chown "$FTP_USER:$FTP_USER" "$FTP_ROOT/$d"
    chmod 755 "$FTP_ROOT/$d"
done

# ---- the certificate ------------------------------------------------
if [ ! -f "$CERT" ]; then
    say "generating a self-signed certificate (10 years)"
    openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
        -subj "/CN=neodct-ftp" \
        -keyout "$CERT" -out "$CERT" >/dev/null 2>&1
    chmod 600 "$CERT"
fi

# ---- the address to advertise in PASV -------------------------------
PUB_IP="${PUB_IP:-$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{print $7; exit}')}"
[ -n "$PUB_IP" ] || { echo "could not work out this host's public IPv4; set PUB_IP=" >&2; exit 1; }

if [ "$WRITABLE" = 1 ]; then WRITE_ENABLE=YES; ALLOW_WR_CHROOT=YES
else WRITE_ENABLE=NO;  ALLOW_WR_CHROOT=NO; fi

say "writing $CONF"
[ -f "$CONF" ] && [ ! -f "$CONF.before-neodct" ] && cp "$CONF" "$CONF.before-neodct"
cat > "$CONF" <<EOF
# Written by neodct/tools/ftp-server-setup.sh. See that script for why.
listen=YES
listen_ipv6=NO
anonymous_enable=NO
local_enable=YES
write_enable=$WRITE_ENABLE
allow_writeable_chroot=$ALLOW_WR_CHROOT
local_umask=022
dirmessage_enable=YES
use_localtime=YES
xferlog_enable=YES
connect_from_port_20=YES
chroot_local_user=YES
secure_chroot_dir=/var/run/vsftpd/empty
pam_service_name=vsftpd
utf8_filesystem=YES

# Only the accounts named here may log in at all.
userlist_enable=YES
userlist_file=/etc/vsftpd.userlist
userlist_deny=NO

# Passive mode, because the phone is behind a carrier NAT and an active-mode
# PORT command from it can never be connected back to.
pasv_enable=YES
pasv_min_port=$PASV_MIN
pasv_max_port=$PASV_MAX
pasv_address=$PUB_IP

# TLS, required. See the header comment for the three lines below it.
ssl_enable=YES
force_local_logins_ssl=YES
force_local_data_ssl=YES
ssl_tlsv1_2=YES
ssl_sslv2=NO
ssl_sslv3=NO
ssl_ciphers=HIGH
rsa_cert_file=$CERT
rsa_private_key_file=$CERT
require_ssl_reuse=NO
seccomp_sandbox=NO

idle_session_timeout=300
data_connection_timeout=300
max_clients=10
max_per_ip=4
EOF

echo "$FTP_USER" > /etc/vsftpd.userlist
mkdir -p /var/run/vsftpd/empty

# ---- the firewall ---------------------------------------------------
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "^Status: active"; then
    say "opening 21/tcp and $PASV_MIN:$PASV_MAX/tcp in ufw"
    ufw allow 21/tcp >/dev/null
    ufw allow "$PASV_MIN:$PASV_MAX/tcp" >/dev/null
else
    say "ufw is not active; if this droplet has a cloud firewall, open 21 and $PASV_MIN-$PASV_MAX"
fi

systemctl enable vsftpd >/dev/null 2>&1 || true
systemctl restart vsftpd
sleep 1
systemctl is-active --quiet vsftpd || { journalctl -u vsftpd -n 30 --no-pager; exit 1; }

FP=$(openssl x509 -in "$CERT" -noout -fingerprint -sha256 | cut -d= -f2)

cat <<EOF

[ftp-setup] up.

  host      $PUB_IP
  user      $FTP_USER
  uploads   $([ "$WRITABLE" = 1 ] && echo "allowed over FTP" || echo "read-only over FTP -- put files there with scp as root")
  folders   $FTP_ROOT/{music,roms,naps,other}
  cert      SHA256 $FP

Put something there and check it from this PC:

  scp some.mp3 root@$PUB_IP:$FTP_ROOT/music/
  curl --ssl-reqd -k -u $FTP_USER ftp://$PUB_IP/music/

Then open Fetch on the phone (engineering mode) and type the password.
EOF
