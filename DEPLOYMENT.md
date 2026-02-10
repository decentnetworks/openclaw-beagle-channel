# Beagle Sidecar Deployment Guide

This guide covers deploying the Beagle Carrier sidecar on Ubuntu for production use.

## Prerequisites

- Ubuntu 20.04 LTS or later
- Root or sudo access
- Beagle Carrier sidecar binary compiled for your platform

## Installation Steps

### 1. Create System User

```bash
sudo useradd -r -s /bin/false -m -d /var/lib/beagle-sidecar openclaw
```

### 2. Install Sidecar Binary

```bash
# Create installation directory
sudo mkdir -p /opt/beagle-sidecar

# Copy your sidecar binary
sudo cp beagle-carrier-sidecar /opt/beagle-sidecar/
sudo chmod +x /opt/beagle-sidecar/beagle-carrier-sidecar

# Set ownership
sudo chown -R openclaw:openclaw /opt/beagle-sidecar
```

### 3. Create Data Directory

```bash
sudo mkdir -p /var/lib/beagle-sidecar
sudo chown openclaw:openclaw /var/lib/beagle-sidecar
sudo chmod 750 /var/lib/beagle-sidecar
```

### 4. Install systemd Service

```bash
# Copy service file
sudo cp beagle-carrier-sidecar.service /etc/systemd/system/

# Edit service file to set your auth token
sudo nano /etc/systemd/system/beagle-carrier-sidecar.service
# Update the AUTH_TOKEN environment variable

# Reload systemd
sudo systemctl daemon-reload
```

### 5. Enable and Start Service

```bash
# Enable service to start on boot
sudo systemctl enable beagle-carrier-sidecar

# Start service
sudo systemctl start beagle-carrier-sidecar

# Check status
sudo systemctl status beagle-carrier-sidecar
```

### 6. Verify Service is Running

```bash
# Check if sidecar is listening on port 39091
curl http://127.0.0.1:39091/events

# View logs
sudo journalctl -u beagle-carrier-sidecar -f
```

## Configuration

### Environment Variables

Edit `/etc/systemd/system/beagle-carrier-sidecar.service` to configure:

- `PORT`: Port number for HTTP API (default: 39091)
- `AUTH_TOKEN`: Shared secret for API authentication
- `LOG_LEVEL`: Logging verbosity (debug, info, warn, error)

After editing, reload:
```bash
sudo systemctl daemon-reload
sudo systemctl restart beagle-carrier-sidecar
```

### Firewall Configuration

If running sidecar on a different machine than OpenClaw:

```bash
# Allow connections from specific IP
sudo ufw allow from <openclaw-ip> to any port 39091

# Or allow from localhost only (recommended)
sudo ufw deny 39091
```

## Monitoring

### View Logs

```bash
# Follow logs in real-time
sudo journalctl -u beagle-carrier-sidecar -f

# View last 100 lines
sudo journalctl -u beagle-carrier-sidecar -n 100

# View logs since specific time
sudo journalctl -u beagle-carrier-sidecar --since "1 hour ago"
```

### Check Service Status

```bash
# Full status
sudo systemctl status beagle-carrier-sidecar

# Is service active?
sudo systemctl is-active beagle-carrier-sidecar

# Is service enabled?
sudo systemctl is-enabled beagle-carrier-sidecar
```

### Resource Usage

```bash
# Check memory/CPU usage
systemctl status beagle-carrier-sidecar

# Detailed resource usage
sudo systemd-cgtop
```

## Maintenance

### Restart Service

```bash
sudo systemctl restart beagle-carrier-sidecar
```

### Stop Service

```bash
sudo systemctl stop beagle-carrier-sidecar
```

### Disable Service

```bash
sudo systemctl disable beagle-carrier-sidecar
```

### Update Sidecar Binary

```bash
# Stop service
sudo systemctl stop beagle-carrier-sidecar

# Backup old binary
sudo cp /opt/beagle-sidecar/beagle-carrier-sidecar /opt/beagle-sidecar/beagle-carrier-sidecar.backup

# Install new binary
sudo cp new-beagle-carrier-sidecar /opt/beagle-sidecar/beagle-carrier-sidecar
sudo chown openclaw:openclaw /opt/beagle-sidecar/beagle-carrier-sidecar
sudo chmod +x /opt/beagle-sidecar/beagle-carrier-sidecar

# Start service
sudo systemctl start beagle-carrier-sidecar

# Verify
sudo systemctl status beagle-carrier-sidecar
```

## Troubleshooting

### Service Won't Start

1. Check logs:
   ```bash
   sudo journalctl -u beagle-carrier-sidecar -n 50 --no-pager
   ```

2. Verify binary permissions:
   ```bash
   ls -l /opt/beagle-sidecar/beagle-carrier-sidecar
   ```

3. Check if port is already in use:
   ```bash
   sudo netstat -tlnp | grep 39091
   ```

### Permission Denied Errors

Ensure data directory is writable:
```bash
sudo chown -R openclaw:openclaw /var/lib/beagle-sidecar
sudo chmod 750 /var/lib/beagle-sidecar
```

### High Memory Usage

Check configuration and consider resource limits in service file:
```ini
[Service]
MemoryMax=512M
MemoryHigh=384M
```

## Security Hardening

### 1. Use Strong Authentication Token

Generate a secure token:
```bash
openssl rand -base64 32
```

Update service file with this token.

### 2. Restrict Network Access

Only allow localhost:
```bash
# In your sidecar configuration
bind_address = "127.0.0.1"
```

### 3. Regular Updates

Keep the sidecar binary up to date with security patches.

### 4. Monitor Logs

Set up log monitoring for suspicious activity:
```bash
sudo journalctl -u beagle-carrier-sidecar | grep -i "error\|fail\|unauthorized"
```

## Advanced Configuration

### Running Multiple Instances

To run multiple sidecar instances on different ports:

```bash
# Copy service file
sudo cp /etc/systemd/system/beagle-carrier-sidecar.service \
        /etc/systemd/system/beagle-carrier-sidecar@.service

# Edit to use instance parameter
Environment="PORT=3909%i"

# Start instances
sudo systemctl start beagle-carrier-sidecar@1
sudo systemctl start beagle-carrier-sidecar@2
```

### Reverse Proxy with Nginx

If you need TLS/SSL:

```nginx
server {
    listen 443 ssl;
    server_name beagle.example.com;

    ssl_certificate /path/to/cert.pem;
    ssl_certificate_key /path/to/key.pem;

    location / {
        proxy_pass http://127.0.0.1:39091;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
```

## Support

For issues:
- Check logs first: `sudo journalctl -u beagle-carrier-sidecar -f`
- GitHub Issues: https://github.com/0xli/openclaw-beagle-channel/issues
