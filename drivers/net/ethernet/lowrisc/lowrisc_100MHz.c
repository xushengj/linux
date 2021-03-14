/*
 * Lowrisc Ether100MHz Linux driver for the Lowrisc Ethernet 100MHz device.
 *
 * This is an experimental driver which is based on the original emac_lite
 * driver from John Williams <john.williams@xilinx.com>.
 *
 * 2007 - 2013 (c) Xilinx, Inc.
 * PHY control portions copyright (C) 2015 Microchip Technology
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/mdio-bitbang.h>
#include <linux/mdio-gpio.h>
#include <linux/version.h>
#include "lowrisc_100MHz.h"

#define DRIVER_AUTHOR	"WOOJUNG HUH <woojung.huh@microchip.com>"
#define DRIVER_DESC	"Microchip LAN8720 PHY driver"
#define DRIVER_NAME     "lowrisc-eth"

/* General Ethernet Definitions */
#define XEL_ARP_PACKET_SIZE		28	/* Max ARP packet size */
#define XEL_HEADER_IP_LENGTH_OFFSET	16	/* IP Length Offset */

#define TX_TIMEOUT		(60*HZ)		/* Tx timeout is 60 seconds. */

/**
 * struct net_local - Our private per device data
 * @ndev:		instance of the network device
 * @reset_lock:		lock used for synchronization
 * @phy_dev:		pointer to the PHY device
 * @phy_node:		pointer to the PHY device node
 * @mii_bus:		pointer to the MII bus
 * @last_link:		last link status
 */
struct net_local {
  struct mdiobb_ctrl ctrl; /* must be first for bitbang driver to work */
  void __iomem *ioaddr;
  struct net_device *ndev;
  u32 msg_enable;
  
  struct phy_device *phy_dev;
  struct mii_bus *mii_bus;
  int last_duplex;
  int last_carrier;
  
  /* Spinlock */
  struct mutex lock;
  uint32_t last_mdio_gpio;
  int irq, num_tests, phy_addr;

  struct napi_struct napi;
};

static void inline eth_write(struct net_local *priv, size_t addr, int data)
{
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  eth_base[addr >> 3] = data;
}

static void inline eth_copyout(struct net_local *priv, uint8_t *data, int len)
{
  int i, rnd = ((len-1)|7)+1;
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  if (!(((size_t)data) & 7))
    {
      uint64_t *ptr = (uint64_t *)data;
      for (i = 0; i < rnd/8; i++)
        eth_base[TXBUFF_OFFSET/8 + i] = ptr[i];
    }
  else // We can't unfortunately rely on the skb being word aligned
    {
      uint64_t notptr;
      for (i = 0; i < rnd/8; i++)
        {
          memcpy(&notptr, data+(i<<3), sizeof(uint64_t));
          eth_base[TXBUFF_OFFSET/8 + i] = notptr;
        }
    }
}

static volatile inline int eth_read(struct net_local *priv, size_t addr)
{
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  return eth_base[addr >> 3];
}

static inline void eth_copyin(struct net_local *priv, uint8_t *data, int len, int start)
{
  int i, rnd = ((len-1)|7)+1;
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  if (!(((size_t)data) & 7))
    {
      uint64_t *ptr = (uint64_t *)data;
      for (i = 0; i < rnd/8; i++)
        ptr[i] = eth_base[start + i];
    }
  else // We can't unfortunately rely on the skb being word aligned
    {
      for (i = 0; i < rnd/8; i++)
        {
          uint64_t notptr = eth_base[start + i];
          memcpy(data+(i<<3), &notptr, sizeof(uint64_t));
        }
    }
}

static void inline eth_enable_irq(struct net_local *priv)
{
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  eth_base[MACHI_OFFSET >> 3] |= MACHI_IRQ_EN;
  mmiowb();
}

static void inline eth_disable_irq(struct net_local *priv)
{
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  eth_base[MACHI_OFFSET >> 3] &= ~MACHI_IRQ_EN;
  mmiowb();
}

/**
 * lowrisc_update_address - Update the MAC address in the device
 * @drvdata:	Pointer to the Ether100MHz device private data
 * @address_ptr:Pointer to the MAC address (MAC address is a 48-bit value)
 *
 * Tx must be idle and Rx should be idle for deterministic results.
 * It is recommended that this function should be called after the
 * initialization and before transmission of any packets from the device.
 * The MAC address can be programmed using any of the two transmit
 * buffers (if configured).
 */

static void lowrisc_update_address(struct net_local *priv, u8 *address_ptr)
{
  uint32_t macaddr_lo, macaddr_hi;
  memcpy (&macaddr_lo, address_ptr+2, sizeof(uint32_t));
  memcpy (&macaddr_hi, address_ptr+0, sizeof(uint16_t));
  mutex_lock(&priv->lock);
  eth_write(priv, MACLO_OFFSET, htonl(macaddr_lo));
  eth_write(priv, MACHI_OFFSET, htons(macaddr_hi));
  eth_write(priv, RFCS_OFFSET, 8); /* use 8 buffers */
  mutex_unlock(&priv->lock);
}

/**
 * lowrisc_read_mac_address - Read the MAC address in the device
 * @drvdata:	Pointer to the Ether100MHz device private data
 * @address_ptr:Pointer to the 6-byte buffer to receive the MAC address (MAC address is a 48-bit value)
 *
 * In lowrisc the starting value is programmed by the boot loader according to DIP switch [15:12]
 */

static void lowrisc_read_mac_address(struct net_local *priv, u8 *address_ptr)
{
  uint32_t macaddr_hi, macaddr_lo;
  mutex_lock(&priv->lock);
  macaddr_hi = ntohs(eth_read(priv, MACHI_OFFSET)&MACHI_MACADDR_MASK);
  macaddr_lo = ntohl(eth_read(priv, MACLO_OFFSET));
  mutex_unlock(&priv->lock);
  memcpy (address_ptr+2, &macaddr_lo, sizeof(uint32_t));
  memcpy (address_ptr+0, &macaddr_hi, sizeof(uint16_t));
}

/**
 * lowrisc_set_mac_address - Set the MAC address for this device
 * @dev:	Pointer to the network device instance
 * @addr:	Void pointer to the sockaddr structure
 *
 * This function copies the HW address from the sockaddr strucutre to the
 * net_device structure and updates the address in HW.
 *
 * Return:	Error if the net device is busy or 0 if the addr is set
 *		successfully
 */
static int lowrisc_set_mac_address(struct net_device *ndev, void *address)
{
	struct net_local *priv = netdev_priv(ndev);
	struct sockaddr *addr = address;
	memcpy(ndev->dev_addr, addr->sa_data, ndev->addr_len);
	lowrisc_update_address(priv, ndev->dev_addr);
	return 0;
}

static void lowrisc_tx_timeout(
  struct net_device *ndev, unsigned int txqueue) __maybe_unused;
static void lowrisc_tx_timeout_wrap(struct net_device *ndev) __maybe_unused;

/**
 * lowrisc_tx_timeout - Callback for Tx Timeout
 * @dev:	Pointer to the network device
 * @txqueue:	Which txqueue is stuck.
 *
 * This function is called when Tx time out occurs for Ether100MHz device.
 */
static void lowrisc_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	/* not using it for now */
	(void) txqueue;

	struct net_local *priv = netdev_priv(ndev);

	dev_err(&priv->ndev->dev, "Exceeded transmit timeout of %lu ms\n",
		TX_TIMEOUT * 1000UL / HZ);

	ndev->stats.tx_errors++;

	/* Reset the device */
	mutex_lock(&priv->lock);

	/* Shouldn't really be necessary, but shouldn't hurt */
	netif_stop_queue(ndev);

	/* To exclude tx timeout */
        netif_trans_update(ndev); /* prevent tx timeout */

	/* We're all ready to go. Start the queue */
	netif_wake_queue(ndev);
	mutex_unlock(&priv->lock);
}

/**
 * lowrisc_tx_timeout_wrap - Callback for Tx Timeout without txqueue parameter
 * @dev:	Pointer to the network device
 *
 * This function is called when Tx time out occurs for Ether100MHz device.
 */
static void lowrisc_tx_timeout_wrap(struct net_device *ndev)
{
  lowrisc_tx_timeout(ndev, 0);
}

/**
 * lowrisc_close - Close the network device
 * @dev:	Pointer to the network device
 *
 * This function stops the Tx queue, disables interrupts and frees the IRQ for
 * the Ether100MHz device.
 * It also disconnects the phy device associated with the Ether100MHz device.
 */
static int lowrisc_close(struct net_device *ndev)
{
	struct net_local *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
        napi_disable(&priv->napi);
        mutex_lock(&priv->lock);
	eth_disable_irq(priv);
        mutex_unlock(&priv->lock);
	if (priv->irq)
		free_irq(priv->irq, priv);
	priv->irq = 0;
        printk("Close device, free interrupt\n");
        
	if (priv->phy_dev)
		phy_disconnect(priv->phy_dev);
	priv->phy_dev = NULL;

	return 0;
}

/**
 * lowrisc_remove_ndev - Free the network device
 * @ndev:	Pointer to the network device to be freed
 *
 * This function un maps the IO region of the Ether100MHz device and frees the net
 * device.
 */
static void lowrisc_remove_ndev(struct net_device *ndev)
{
	if (ndev) {
		free_netdev(ndev);
	}
}

static void lowrisc_phy_adjust_link(struct net_device *ndev)
{
	struct net_local *priv = netdev_priv(ndev);
	struct phy_device *phy_dev = priv->phy_dev;
	int carrier;

	if (phy_dev->duplex != priv->last_duplex) {
		if (phy_dev->duplex) {
			netif_dbg(priv, link, priv->ndev, "full duplex mode\n");
		} else {
			netif_dbg(priv, link, priv->ndev, "half duplex mode\n");
		}

		priv->last_duplex = phy_dev->duplex;
	}

	carrier = netif_carrier_ok(ndev);
	if (carrier != priv->last_carrier) {
		if (carrier)
			netif_dbg(priv, link, priv->ndev, "carrier OK\n");
		else
			netif_dbg(priv, link, priv->ndev, "no carrier\n");
		priv->last_carrier = carrier;
	}
}

static int lowrisc_mii_probe(struct net_device *ndev)
{
	struct net_local *priv = netdev_priv(ndev);
	struct phy_device *phydev = NULL;
	const char *phyname;

	BUG_ON(priv->phy_dev);

	/* find the first (lowest address) PHY
	 * on the current MAC's MII bus
	 */
	for (priv->phy_addr = 1; priv->phy_addr < PHY_MAX_ADDR; priv->phy_addr++)
          {
            phydev = mdiobus_get_phy(priv->mii_bus, priv->phy_addr);
            if (phydev) {
              /* break out with first one found */
              break;
            }
          }
	if (!phydev) {
          netdev_err(ndev, "no PHY found in range address 0..%d\n", PHY_MAX_ADDR-1);
		return -ENODEV;
	}

	phyname = phydev_name(phydev);
	printk("Probing %s (address %d)\n", phyname, priv->phy_addr);
	
	phydev = phy_connect(ndev, phyname,
			     lowrisc_phy_adjust_link, PHY_INTERFACE_MODE_MII);

	if (IS_ERR(phydev)) {
		netdev_err(ndev, "Could not attach to PHY\n");
		return PTR_ERR(phydev);
	}

	/* mask with MAC supported features */
	linkmode_copy(phydev->advertising, phydev->supported);

	phy_attached_info(phydev);

	priv->phy_dev = phydev;
	priv->last_duplex = -1;
	priv->last_carrier = -1;

	return 0;
}

static void mdio_dir(struct mdiobb_ctrl *ctrl, int dir)
{
  struct net_local *priv = (struct net_local *)ctrl; /* struct mdiobb_ctrl must be first in net_local for bitbang driver to work */
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  if (dir)
    priv->last_mdio_gpio |= MDIOCTRL_MDIOOEN_MASK; // output driving
  else
    priv->last_mdio_gpio &= ~MDIOCTRL_MDIOOEN_MASK; // input receiving
  eth_base[MDIOCTRL_OFFSET >> 3] = priv->last_mdio_gpio;
  mmiowb();
}

static int mdio_get(struct mdiobb_ctrl *ctrl)
{
  int retval;
  struct net_local *priv = (struct net_local *)ctrl; /* struct mdiobb_ctrl must be first in net_local for bitbang driver to work */
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  retval = eth_base[MDIOCTRL_OFFSET >> 3];
  return retval & MDIOCTRL_MDIOIN_MASK ? 1:0;
}

static void mdio_set(struct mdiobb_ctrl *ctrl, int what)
{
  struct net_local *priv = (struct net_local *)ctrl; /* struct mdiobb_ctrl must be first in net_local for bitbang driver to work */
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  if (what)
    priv->last_mdio_gpio |= MDIOCTRL_MDIOOUT_MASK;
  else
    priv->last_mdio_gpio &= ~MDIOCTRL_MDIOOUT_MASK;
  eth_base[MDIOCTRL_OFFSET >> 3] = priv->last_mdio_gpio;
  mmiowb();
}

static void mdc_set(struct mdiobb_ctrl *ctrl, int what)
{
  struct net_local *priv = (struct net_local *)ctrl; /* struct mdiobb_ctrl must be first in net_local for bitbang driver to work */
  volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);
  if (what)
    priv->last_mdio_gpio |= MDIOCTRL_MDIOCLK_MASK;
  else
    priv->last_mdio_gpio &= ~MDIOCTRL_MDIOCLK_MASK;
  eth_base[MDIOCTRL_OFFSET >> 3] = priv->last_mdio_gpio;
  mmiowb();
}

static struct mdiobb_ops mdio_gpio_ops = {
        .owner = THIS_MODULE,
        .set_mdc = mdc_set,
        .set_mdio_dir = mdio_dir,
        .set_mdio_data = mdio_set,
        .get_mdio_data = mdio_get,
};

static int lowrisc_mii_init(struct net_device *ndev)
{
        struct mii_bus *new_bus;
	struct net_local *priv = netdev_priv(ndev);
	int err = -ENXIO;
	
	priv->ctrl.ops = &mdio_gpio_ops;
        new_bus = alloc_mdio_bitbang(&(priv->ctrl));

	if (!new_bus) {
		err = -ENOMEM;
		goto err_out_1;
	}
	snprintf(new_bus->id, MII_BUS_ID_SIZE, "lowrisc-0");
        new_bus->name = "GPIO Bitbanged LowRISC",

        new_bus->phy_mask = ~(1 << 1);
        new_bus->phy_ignore_ta_mask = 0;

	mutex_init(&(new_bus->mdio_lock));
	
	priv->mii_bus = new_bus;
	priv->mii_bus->priv = priv;

	/* Mask all PHYs except ID 1 (internal) */
	priv->mii_bus->phy_mask = ~(1 << 1);

	if (mdiobus_register(priv->mii_bus)) {
		netif_warn(priv, probe, priv->ndev, "Error registering mii bus\n");
		goto err_out_free_bus_2;
	}

	if (lowrisc_mii_probe(ndev) < 0) {
		netif_warn(priv, probe, priv->ndev, "Error probing mii bus\n");
		goto err_out_unregister_bus_3;
	}

	return 0;

err_out_unregister_bus_3:
	mdiobus_unregister(priv->mii_bus);
err_out_free_bus_2:
	mdiobus_free(priv->mii_bus);
err_out_1:
	return err;
}
/**********************/
/* Interrupt Handlers */
/**********************/

/**
 * lowrisc_ether_isr - Interrupt handler for frames received
 * @priv:	Pointer to the struct net_local
 *
 * This function allocates memory for a socket buffer, fills it with data
 * received and hands it over to the TCP/IP stack.
 */

static int lowrisc_ether_poll(struct napi_struct *napiptr, int budget)
{
  int rsr, buf, rx_count = 0;
  struct net_local *priv = container_of(napiptr, struct net_local, napi);
  struct net_device *ndev = priv->ndev;
  rsr = eth_read(priv, RSR_OFFSET);
  buf = rsr & RSR_RECV_FIRST_MASK;
  /* Check if there is Rx Data available */
  while ((rsr & RSR_RECV_DONE_MASK) && (rx_count < budget))
    {
      int len = eth_read(priv, RPLR_OFFSET+((buf&7)<<3)) - 4; /* discard FCS bytes ?? */
      if ((len >= 60) && (len <= ETH_FRAME_LEN + ETH_FCS_LEN))
	{
	  int rnd = ((len-1)|7)+1; /* round to a multiple of 8 */
	  struct sk_buff *skb = __napi_alloc_skb(&priv->napi, rnd, GFP_ATOMIC|__GFP_NOWARN); // Don't warn, just drop surplus packets
	  if (unlikely(!skb))
	    {
	      /* Couldn't get memory, we carry on regardless and drop if necessary */
	      ndev->stats.rx_dropped++;
	    }
	  else
	    {
	      int start = RXBUFF_OFFSET/8 + ((buf&7)<<8);
              skb_put(skb, len);	/* Tell the skb how much data we got */
	      
              eth_copyin(priv, skb->data, len, start);
              skb->protocol = eth_type_trans(skb, ndev);
              netif_receive_skb(skb);
              ndev->stats.rx_packets++;
              ndev->stats.rx_bytes += len;
              ++rx_count;
            }
        }
      else
	  ndev->stats.rx_errors++;
      /* acknowledge, even if an error occurs, to reset irq */
      eth_write(priv, RSR_OFFSET, ++buf);
      rsr = eth_read(priv, RSR_OFFSET);
    }

  if (rsr & RSR_RECV_DONE_MASK)
    {
      return 1;
    }
  else
    {
      napi_complete_done(&priv->napi, rx_count);
      eth_enable_irq(priv);
      return 0;
    }
}

irqreturn_t lowrisc_ether_isr(int irq, void *priv_id)
{
  int rsr = 0;
  struct net_local *priv = priv_id;
  struct net_device *ndev = priv->ndev;
  if (napi_schedule_prep(&priv->napi))
    {
      eth_disable_irq(priv);
      __napi_schedule(&priv->napi);
    }
  else /* we are in denial of service mode */
    do
      {
        int buf = rsr & RSR_RECV_FIRST_MASK;
        ndev->stats.rx_dropped++;            
        eth_write(priv, RSR_OFFSET, ++buf);
        rsr = eth_read(priv, RSR_OFFSET);
      }
    while (rsr & RSR_RECV_DONE_MASK);
  return IRQ_HANDLED;
}

static int lowrisc_get_regs_len(struct net_device __always_unused *netdev)
{
#define LOWRISC_REGS_LEN 64	/* overestimate */
  return LOWRISC_REGS_LEN * sizeof(u32);
}

static void lowrisc_get_regs(struct net_device *ndev,
			   struct ethtool_regs *regs, void *p)
{
  struct net_local *priv = netdev_priv(ndev);
  struct phy_device *phy = priv->phy_dev;

  u32 *regs_buff = p;
  int i;

  memset(p, 0, LOWRISC_REGS_LEN * sizeof(u32));

  regs->version = 0;
  mutex_lock(&priv->lock);
  for (i = 0; i < LOWRISC_REGS_LEN; i++)
    {
      if (i >= 32)
	regs_buff[i] = eth_read(priv, MACLO_OFFSET+((i-32)<<3));
      else
	{
	regs_buff[i] = phy_read(phy, i);
	}
    }
  mutex_unlock(&priv->lock);
}

static const struct ethtool_ops lowrisc_ethtool_ops = {
	.get_regs_len		= lowrisc_get_regs_len,
	.get_regs		= lowrisc_get_regs,
        .get_link_ksettings     = phy_ethtool_get_link_ksettings,
        .set_link_ksettings     = phy_ethtool_set_link_ksettings        
};

/**
 * lowrisc_open - Open the network device
 * @dev:	Pointer to the network device
 *
 * This function sets the MAC address, requests an IRQ and enables interrupts
 * for the Ether100MHz device and starts the Tx queue.
 * It also connects to the phy device, if MDIO is included in Ether100MHz device.
 */

static int lowrisc_open(struct net_device *ndev)
{
  int retval = 0;
  struct net_local *priv = netdev_priv(ndev);
  ndev->ethtool_ops = &lowrisc_ethtool_ops;

  /* Set the MAC address each time opened */
  lowrisc_update_address(priv, ndev->dev_addr);
  
  if (priv->phy_dev) {
    linkmode_copy(priv->phy_dev->advertising, priv->phy_dev->supported);
    phy_start(priv->phy_dev);
  }
  
  /* Grab the IRQ */
  printk("Open device, request interrupt %d\n", priv->irq);
  retval = request_irq(priv->irq, lowrisc_ether_isr, 0, ndev->name, priv);
  if (retval) {
    dev_err(&priv->ndev->dev, "Could not allocate interrupt %d\n", priv->irq);
    if (priv->phy_dev)
      phy_disconnect(priv->phy_dev);
    priv->phy_dev = NULL;
    return retval;
  }
  
  lowrisc_update_address(priv, ndev->dev_addr);

  /* We're ready to go */
  napi_enable(&priv->napi);
  netif_start_queue(ndev);

  /* enable the irq */
  eth_enable_irq(priv);
  return 0;
}

/**
 * lowrisc_send - Transmit a frame
 * @orig_skb:	Pointer to the socket buffer to be transmitted
 * @dev:	Pointer to the network device
 *
 * This function checks if the Tx buffer of the Ether100MHz device is free to send
 * data. If so, it fills the Tx buffer with data from socket buffer data,
 * updates the stats and frees the socket buffer.
 * Return:	0, always.
 */
static netdev_tx_t lowrisc_send(struct sk_buff *new_skb, struct net_device *ndev)
{
	struct net_local *priv = netdev_priv(ndev);
	unsigned int len = new_skb->len;
        int rslt;
	if (mutex_is_locked(&priv->lock))
          return NETDEV_TX_BUSY;
        rslt = eth_read(priv, TPLR_OFFSET);
        if (rslt & TPLR_BUSY_MASK)
          return NETDEV_TX_BUSY;
        eth_copyout(priv, new_skb->data, len);
        eth_write(priv, TPLR_OFFSET, len);

	skb_tx_timestamp(new_skb);

	ndev->stats.tx_bytes += len;
	ndev->stats.tx_packets++;
	dev_consume_skb_any(new_skb);

	return NETDEV_TX_OK;
}

static int lowrisc_mii_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
        struct net_local *priv = netdev_priv(netdev);
	struct phy_device *phy = priv->phy_dev;
        struct mii_ioctl_data *data = if_mii(ifr);

        switch (cmd) {
        case SIOCGMIIPHY:
                data->phy_id = 1;
                break;
        case SIOCGMIIREG:
                data->val_out = phy_read(phy, data->reg_num);
                break;
        case SIOCSMIIREG:
                phy_write(phy, data->reg_num, data->val_in);
                break;
        default:
                return -EOPNOTSUPP;
        }
        return 0;
	}

static struct net_device_stats *lowrisc_get_stats(struct net_device *ndev)
{
        return &ndev->stats;
}

static void lowrisc_set_rx_mode(struct net_device *ndev)
{
	struct net_local *priv = netdev_priv(ndev);
        volatile uint64_t *eth_base = (volatile uint64_t *)(priv->ioaddr);

        if (ndev->flags & IFF_PROMISC)
          {
            /* Set promiscuous. */
            eth_base[MACHI_OFFSET >> 3] |= MACHI_ALLPKTS_MASK;
          }
        else
          {
            eth_base[MACHI_OFFSET >> 3] &= ~MACHI_ALLPKTS_MASK;            
          }
}
static struct net_device_ops lowrisc_netdev_ops = {
	.ndo_open		= lowrisc_open,
	.ndo_stop		= lowrisc_close,
	.ndo_start_xmit		= lowrisc_send,
        .ndo_get_stats          = lowrisc_get_stats,
        .ndo_set_rx_mode        = lowrisc_set_rx_mode,
	.ndo_set_mac_address	= lowrisc_set_mac_address,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
	.ndo_tx_timeout		= lowrisc_tx_timeout,
#else
	.ndo_tx_timeout		= lowrisc_tx_timeout_wrap,
#endif
	.ndo_do_ioctl           = lowrisc_mii_ioctl,
};

/**
 * lowrisc_of_probe - Probe method for the Ether100MHz device.
 * @ofdev:	Pointer to OF device structure
 * @match:	Pointer to the structure used for matching a device
 *
 * This function probes for the Ether100MHz device in the device tree.
 * It initializes the driver data structure and the hardware, sets the MAC
 * address and registers the network device.
 * It also registers a mii_bus for the Ether100MHz device, if MDIO is included
 * in the device.
 *
 * Return:	0, if the driver is bound to the Ether100MHz device, or
 *		a negative error if there is failure.
 */
static int lowrisc_100MHz_probe(struct platform_device *ofdev)
{
	struct net_device *ndev = NULL;
	struct net_local *priv = NULL;
	struct device *dev = &ofdev->dev;
        struct resource *lowrisc_ethernet;
	unsigned char mac_address[7];
	int rc = 0;

        lowrisc_ethernet = platform_get_resource(ofdev, IORESOURCE_MEM, 0);

	/* Create an ethernet device instance */
	ndev = alloc_etherdev(sizeof(struct net_local));
	if (!ndev)
		return -ENOMEM;

	dev_set_drvdata(dev, ndev);
	SET_NETDEV_DEV(ndev, &ofdev->dev);
        platform_set_drvdata(ofdev, ndev);
        
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
        priv->ioaddr = devm_ioremap_resource(&ofdev->dev, lowrisc_ethernet);
	mutex_init(&priv->lock);

        priv->num_tests = 1;
        
	ndev->netdev_ops = &lowrisc_netdev_ops;
	ndev->flags &= ~IFF_MULTICAST;
	ndev->watchdog_timeo = TX_TIMEOUT;
        netif_napi_add(ndev, &priv->napi, lowrisc_ether_poll, 64);

	printk("lowrisc-digilent-ethernet: Lowrisc ethernet platform (%llX-%llX) mapped to %lx\n",
               lowrisc_ethernet[0].start,
               lowrisc_ethernet[0].end,
               (size_t)(priv->ioaddr));

        priv->irq = platform_get_irq(ofdev, 0);
        
        /* get the MAC address set by the boot loader */
        lowrisc_read_mac_address(priv, mac_address);
	memcpy(ndev->dev_addr, mac_address, ETH_ALEN);

	/* Set the MAC address in the Ether100MHz device */
	lowrisc_update_address(priv, ndev->dev_addr);

	lowrisc_mii_init(ndev);

	/* Finally, register the device */
	rc = register_netdev(ndev);
	if (rc) {
          dev_err(dev,
                  "Cannot register network device, aborting\n");
          goto error;
	}

	dev_info(dev, "Lowrisc Ether100MHz registered\n");
	
	return 0;

error:
	lowrisc_remove_ndev(ndev);
	return rc;
}

/* Match table for OF platform binding */
static const struct of_device_id lowrisc_100MHz_of_match[] = {
	{ .compatible = DRIVER_NAME },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, lowrisc_100MHz_of_match);

void lowrisc_100MHz_free(struct platform_device *of_dev)
{
        struct resource *iomem = platform_get_resource(of_dev, IORESOURCE_MEM, 0);
        release_mem_region(iomem->start, resource_size(iomem));
}

int lowrisc_100MHz_unregister(struct platform_device *of_dev)
{
        lowrisc_100MHz_free(of_dev);
        return 0;
}

static struct platform_driver lowrisc_100MHz_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = lowrisc_100MHz_of_match,
	},
	.probe = lowrisc_100MHz_probe,
	.remove = lowrisc_100MHz_unregister,
};

module_platform_driver(lowrisc_100MHz_driver);

MODULE_AUTHOR("Jonathan Kimmitt");
MODULE_DESCRIPTION("Lowrisc Ethernet 100MHz driver");
MODULE_LICENSE("GPL");
