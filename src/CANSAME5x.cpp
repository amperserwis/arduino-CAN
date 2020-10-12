// Copyright 2020 © Jeff Epler for Adafruit Industries. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdint.h>
#include <stdlib.h>

#include "CANSAME5x.h"
#include "wiring_private.h"

#include "same51.h"

#define hw (reinterpret_cast<Can *>(this->_hw))
#define state (reinterpret_cast<_canSAME5x_state *>(this->_state))

#define DIV_ROUND(a, b) (((a) + (b) / 2) / (b))
#define DIV_ROUND_UP(a, b) (((a) + (b)-1) / (b))

#define GCLK_CAN1 GCLK_PCHCTRL_GEN_GCLK1_Val
#define ADAFRUIT_ZEROCAN_TX_BUFFER_SIZE (1)
#define ADAFRUIT_ZEROCAN_RX_FILTER_SIZE (14)
#define ADAFRUIT_ZEROCAN_RX_FIFO_SIZE (8)
#define ADAFRUIT_ZEROCAN_MAX_MESSAGE_LENGTH (8)

#define CAN0_FUNCTION (EPioType(8))
#define CAN1_FUNCTION (EPioType(7))

namespace
{
// This appears to be a typo (transposition error) in the ASF4 headers
// It's called the "Extended ID Filter Entry"
typedef CanMramXifde CanMramXidfe;

typedef uint32_t can_filter_t;

struct _canSAME5x_tx_buf {
  CAN_TXBE_0_Type txb0;
  CAN_TXBE_1_Type txb1;
  __attribute__((aligned(4))) uint8_t data[8];
};

struct _canSAME5x_rx_fifo
{
  CAN_RXF0E_0_Type rxf0;
  CAN_RXF0E_1_Type rxf1;
  __attribute((aligned(4))) uint8_t data[ADAFRUIT_ZEROCAN_MAX_MESSAGE_LENGTH];
} can_rx_fifo_t;

struct _canSAME5x_state
{
  _canSAME5x_tx_buf tx_buffer[ADAFRUIT_ZEROCAN_TX_BUFFER_SIZE];
  _canSAME5x_rx_fifo rx0_fifo[ADAFRUIT_ZEROCAN_RX_FIFO_SIZE];
  _canSAME5x_rx_fifo rx1_fifo[ADAFRUIT_ZEROCAN_RX_FIFO_SIZE];
  CanMramSidfe standard_rx_filter[ADAFRUIT_ZEROCAN_RX_FILTER_SIZE];
  CanMramXifde extended_rx_filter[ADAFRUIT_ZEROCAN_RX_FILTER_SIZE];
};

// This data must be in the first 64kB of RAM.  The "canram" section
// receives special support from the linker file in the Feather M4 CAN's
// board support package.
// TODO support CAN0 and CAN1 simultaneously (state would be an array of 2)
__attribute__((section("canram"))) _canSAME5x_state can_state;

constexpr uint32_t can_frequency = VARIANT_GCLK1_FREQ;
bool compute_nbtp(uint32_t baudrate, CAN_NBTP_Type &result)
{
  uint32_t clocks_per_bit = DIV_ROUND(can_frequency, baudrate);
  uint32_t clocks_to_sample = DIV_ROUND(clocks_per_bit * 7, 8);
  uint32_t clocks_after_sample = clocks_per_bit - clocks_to_sample;
  uint32_t divisor = max(DIV_ROUND_UP(clocks_to_sample, 256),
                         DIV_ROUND_UP(clocks_after_sample, 128));
  if (divisor > 32) {
    return false;
  }
  result.bit.NTSEG1 = DIV_ROUND(clocks_to_sample, divisor) - 2;
  result.bit.NTSEG2 = DIV_ROUND(clocks_after_sample, divisor) - 1;
  result.bit.NBRP = divisor - 1;
  result.bit.NSJW = DIV_ROUND(clocks_after_sample, divisor * 4);
  return true;
}
} // namespace

CANSAME5x::CANSAME5x(uint8_t TX_PIN, uint8_t RX_PIN)
    : _tx(TX_PIN), _rx(RX_PIN) {}
#ifdef PIN_CAN_TX
CANSAME5x::CANSAME5x() : _tx(PIN_CAN_TX), _rx(PIN_CAN_RX) {}
#else
CANSAME5x::CANSAME5x() : _tx(-1) {}
#endif

CANSAME5x::~CANSAME5x() {}

int CANSAME5x::begin(long baudrate) {
  if (_tx == -1) {
    return 0;
  }

  CAN_NBTP_Type nbtp;
  if (!compute_nbtp(baudrate, nbtp)) {
    return 0;
  }

  // TODO: Support the CAN0 peripheral, which uses pinmux 8
  _hw = reinterpret_cast<void *>(CAN1);
  _state = reinterpret_cast<void *>(&can_state);
  memset(state, 0, sizeof(*state));

  pinPeripheral(_tx, CAN1_FUNCTION);
  pinPeripheral(_rx, CAN1_FUNCTION);

  GCLK->PCHCTRL[CAN1_GCLK_ID].reg = GCLK_CAN1 | (1 << GCLK_PCHCTRL_CHEN_Pos);

  // reset and allow configuration change
  hw->CCCR.bit.INIT = 1;
  while (!hw->CCCR.bit.INIT) {
  }
  hw->CCCR.bit.CCE = 1;

#if 0
  // XXX - set loopback and silent modes
  hw->CCCR.bit.MON = silent;
  hw->CCCR.bit.TEST = loopback;
  hw->TEST.bit.LBCK = loopback;
#endif

  // All TX data has an 8 byte payload (max)
  {
    CAN_TXESC_Type esc = {};
    esc.bit.TBDS = CAN_TXESC_TBDS_DATA8_Val;
    CAN1->TXESC.reg = esc.reg;
  }

  // Set up TX buffer
  {
    CAN_TXBC_Type bc = {};
    bc.bit.TBSA = (uint32_t)state->tx_buffer;
    bc.bit.NDTB = ADAFRUIT_ZEROCAN_TX_BUFFER_SIZE;
    bc.bit.TFQM = 0; // Messages are transmitted in the order submitted
    CAN1->TXBC.reg = bc.reg;
  }

  // All RX data has an 8 byte payload (max)
  {
    CAN_RXESC_Type esc = {};
    esc.bit.F0DS = CAN_RXESC_F0DS_DATA8_Val;
    esc.bit.F1DS = CAN_RXESC_F1DS_DATA8_Val;
    esc.bit.RBDS = CAN_RXESC_RBDS_DATA8_Val;
    hw->RXESC.reg = esc.reg;
  }

  // Set up RX fifo 0
  {
    CAN_RXF0C_Type rxf = {};
    rxf.bit.F0SA = (uint32_t)state->rx0_fifo;
    rxf.bit.F0S = ADAFRUIT_ZEROCAN_RX_FIFO_SIZE;
    hw->RXF0C.reg = rxf.reg;
  }

  // Set up RX fifo 1
  {
    CAN_RXF1C_Type rxf = {};
    rxf.bit.F1SA = (uint32_t)state->rx1_fifo;
    rxf.bit.F1S = ADAFRUIT_ZEROCAN_RX_FIFO_SIZE;
    hw->RXF1C.reg = rxf.reg;
  }

  // Reject all packets not explicitly requested
  {
    CAN_GFC_Type gfc = {};
    gfc.bit.RRFE = 0;
    gfc.bit.ANFS = CAN_GFC_ANFS_REJECT_Val;
    gfc.bit.ANFE = CAN_GFC_ANFE_REJECT_Val;
    hw->GFC.reg = gfc.reg;
  }

  // Set up standard RX filters
  {
    CAN_SIDFC_Type dfc = {};
    dfc.bit.LSS = ADAFRUIT_ZEROCAN_RX_FILTER_SIZE;
    dfc.bit.FLSSA = (uint32_t)state->standard_rx_filter;
    hw->SIDFC.reg = dfc.reg;
  }

  // Set up extended RX filters
  {
    CAN_XIDFC_Type dfc = {};
    dfc.bit.LSE = ADAFRUIT_ZEROCAN_RX_FILTER_SIZE;
    dfc.bit.FLESA = (uint32_t)state->extended_rx_filter;
    hw->XIDFC.reg = dfc.reg;
  }

  // Set nominal baud rate
  hw->NBTP.reg = nbtp.reg;

  // hardware is ready for use
  CAN1->CCCR.bit.CCE = 0;
  CAN1->CCCR.bit.INIT = 0;
  while (CAN1->CCCR.bit.INIT) {
  }

  return 1;
}

void CANSAME5x::end()
{
  // TODO
}

int CANSAME5x::endPacket()
{
  if (!CANControllerClass::endPacket()) {
    return 0;
  }

  // TODO wait for TX buffer to free

  _canSAME5x_tx_buf &buf = state->tx_buffer[0];
  buf.txb0.bit.ESI = false;
  buf.txb0.bit.XTD = _txExtended;
  buf.txb0.bit.RTR = _txRtr;
  if (_txExtended) {
    buf.txb0.bit.ID = _txId;
  } else {
    buf.txb0.bit.ID = _txId << 18;
  }
  buf.txb1.bit.MM = 0;
  buf.txb1.bit.EFC = 0;
  buf.txb1.bit.FDF = 0;
  buf.txb1.bit.BRS = 0;
  buf.txb1.bit.DLC = _txLength;

  if (!_txRtr) {
    memcpy(buf.data, _txData, _txLength);
  }

  // TX buffer add request
  hw->TXBAR.reg = 1;

  // wait 8ms (hard coded for now) for TX to occur
  for (int i = 0; i < 8000; i++) {
    if (hw->TXBTO.reg & 1) {
      return true;
    }
    yield();
  }

  return 1;
}

int CANSAME5x::parsePacket()
{
// TODO
    return 0;
}

void CANSAME5x::onReceive(void(*callback)(int))
{
  CANControllerClass::onReceive(callback);
// TODO: finish implementation
}

int CANSAME5x::filter(int id, int mask)
{
//TODO
}

int CANSAME5x::filterExtended(long id, long mask)
{
//TODO
}

int CANSAME5x::observe() {
  hw->CCCR.bit.INIT = 1;
  while (!hw->CCCR.bit.INIT) {
  }
  hw->CCCR.bit.CCE = 1;

  hw->CCCR.bit.MON = 1;

  CAN1->CCCR.bit.CCE = 0;
  CAN1->CCCR.bit.INIT = 0;
  while (CAN1->CCCR.bit.INIT) {
  }
  return 1;
}


int CANSAME5x::loopback() {
  hw->CCCR.bit.INIT = 1;
  while (!hw->CCCR.bit.INIT) {
  }
  hw->CCCR.bit.CCE = 1;

  hw->CCCR.bit.TEST = 1;
  hw->TEST.bit.LBCK = 1;

  CAN1->CCCR.bit.CCE = 0;
  CAN1->CCCR.bit.INIT = 0;
  while (CAN1->CCCR.bit.INIT) {
  }
  return 1;
}

int CANSAME5x::sleep() {
  return 1;
}

int CANSAME5x::wakeup() {
  return 1;
}
