# kernel-otp

A One-Time Password (OTP) Linux kernel module **and** companion Rust CLI tool.

---

## Prérequisites

1. **Kernel headers & build tools**

   ```sh
   sudo apt-get update
   sudo apt-get install -y build-essential libncurses-dev bison flex \
       libssl-dev libelf-dev linux-headers-$(uname -r)
   ```
2. **Rust & Cargo** (via rustup)

   ```sh
   curl https://sh.rustup.rs -sSf | sh   # choose "default toolchain"
   source ~/.cargo/env
   ```

---

## 1. Build and install the CLI tool

```sh
# From repo root
git clone https://github.com/mathis-la-debrouille/kernel-otp.git
cd kernel-otp

# Build Rust CLI
tools: otp_tool
directory: otp_tool
cargo build --release --manifest-path otp_tool/Cargo.toml
sudo install otp_tool/target/release/otp_tool /usr/local/bin/

# Verify
otp_tool --help
```

---

## 2. Build the kernel module

```sh
# From repo root
make -C otp
```

Compiled module: `otp/otp.ko`

---

## 3. udev rule for user access

By default `/dev/otp*` is `root:root 0600`. To allow non-root access:

```sh
echo 'KERNEL=="otp[0-9]*", MODE="0666"' | sudo tee /etc/udev/rules.d/99-otp.rules
sudo udevadm control --reload-rules
```

Optionally reload module:

```sh
sudo rmmod otp
sudo insmod otp/otp.ko devices=3 pwd_list=p4ssw0rd,12345,kernel,qwerty
```

or manually:

```sh
sudo chmod 666 /dev/otp*
```

---

## 4. Load the module

```sh
sudo insmod otp/otp.ko devices=3 pwd_list=p4ssw0rd,12345,kernel,qwerty
```

* `devices=3` → `/dev/otp0`, `/dev/otp1`, `/dev/otp2`
* `pwd_list=…` → passwords for **list** mode

Verify:

```sh
ls /dev/otp*
# crw-rw-rw- … /dev/otp0 otp1 otp2
```

---

## 5. Usage

### 5.1 Display device status

* **Raw**:

  ```sh
  cat /proc/otp
  ```
* **CLI tool**:

  ```sh
  otp_tool show
  ```

### 5.2 Request an OTP

* **Raw**:

  ```sh
  cat /dev/otp0; echo
  ```
* **CLI tool**:

  ```sh
  otp_tool get-token /dev/otp0
  ```

### 5.3 Validate an OTP

* **Raw**:

  ```sh
  # Wrong OTP
  echo -n "wrong" > /dev/otp0  # → Invalid argument

  # Correct OTP
  echo -n "p4ssw0rd" > /dev/otp0

  # Replaying same OTP fails
  echo -n "p4ssw0rd" > /dev/otp0  # → Invalid argument
  ```
* **CLI tool**:

  ```sh
  otp_tool verify /dev/otp0 p4ssw0rd
  # → Token verified for system path '/dev/otp0'
  ```

---

## 6. Hot reconfiguration via CLI

1. **Change number of devices**:

   ```sh
   otp_tool update-count 5
   ls /dev/ | grep otp  # otp0…otp4
   ```
2. **Update password list**:

   ```sh
   otp_tool update-security new one more
   otp_tool display-security  # displays "new,one,more"
   ```
3. **Switch device mode**:

   ```sh
   # static mode
   otp_tool modify /dev/otp0 static

   # dynamic mode
   otp_tool modify /dev/otp0 dynamic
   ```

---

## 7. Cleanup

```sh
sudo rmmod otp
sudo rm /etc/udev/rules.d/99-otp.rules
sudo udevadm control --reload-rules
```

---