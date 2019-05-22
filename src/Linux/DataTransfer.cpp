/*
 * DataTransfer.cpp
 *
 *  Created on: 29 Mar 2019
 *      Author: Christian
 */

#include <algorithm>

#include "DataTransfer.h"

#include "RepRapFirmware.h"
#include "GCodes/GCodeMachineState.h"
#include "Movement/Move.h"
#include "Movement/BedProbing/Grid.h"
#include "OutputMemory.h"
#include "RepRap.h"

#if HAS_LINUX_INTERFACE

# include "xdmac/xdmac.h"

# define LINUX_SPI				SPI1
# define LINUX_SPI_ID			ID_SPI1
# define LINUX_SPI_IRQn			SPI1_IRQn
# define LINUX_SPI_HANDLER		SPI1_Handler

// XDMAC hardware, see datasheet
const uint32_t LINUX_XDMAC_TX_CH_NUM = 3;
const uint32_t LINUX_XDMAC_RX_CH_NUM = 4;

static xdmac_channel_config_t xdmac_tx_cfg, xdmac_rx_cfg;

volatile bool dataReceived = false;
volatile unsigned int spiTxUnderruns = 0, spiRxOverruns = 0;

void setup_spi(void *inBuffer, const void *outBuffer, size_t bytesToTransfer)
{
	// Reset SPI
	spi_reset(LINUX_SPI);
	spi_set_slave_mode(LINUX_SPI);
	spi_disable_mode_fault_detect(LINUX_SPI);
	spi_set_peripheral_chip_select_value(LINUX_SPI, spi_get_pcs(0));
	spi_set_clock_polarity(LINUX_SPI, 0, 0);
	spi_set_clock_phase(LINUX_SPI, 0, 1);
	spi_set_bits_per_transfer(LINUX_SPI, 0, SPI_CSR_BITS_8_BIT);

	// Initialize channel config for transmitter
	xdmac_tx_cfg.mbr_ubc = bytesToTransfer;
	xdmac_tx_cfg.mbr_sa = (uint32_t)outBuffer;
	xdmac_tx_cfg.mbr_da = (uint32_t)&(LINUX_SPI->SPI_TDR);
	xdmac_tx_cfg.mbr_cfg = XDMAC_CC_TYPE_PER_TRAN |
		XDMAC_CC_MBSIZE_SINGLE |
		XDMAC_CC_DSYNC_MEM2PER |
		XDMAC_CC_CSIZE_CHK_1 |
		XDMAC_CC_DWIDTH_BYTE |
		XDMAC_CC_SIF_AHB_IF0 |
		XDMAC_CC_DIF_AHB_IF1 |
		XDMAC_CC_SAM_INCREMENTED_AM |
		XDMAC_CC_DAM_FIXED_AM |
		XDMAC_CC_PERID(LINUX_XDMAC_TX_CH_NUM);
	xdmac_tx_cfg.mbr_bc = 0;
	xdmac_tx_cfg.mbr_ds = 0;
	xdmac_tx_cfg.mbr_sus = 0;
	xdmac_tx_cfg.mbr_dus = 0;
	xdmac_configure_transfer(XDMAC, DmacChanLinuxTx, &xdmac_tx_cfg);

	xdmac_channel_set_descriptor_control(XDMAC, DmacChanLinuxTx, 0);
	xdmac_channel_enable(XDMAC, DmacChanLinuxTx);
	xdmac_disable_interrupt(XDMAC, DmacChanLinuxTx);

	// Initialize channel config for receiver
	xdmac_rx_cfg.mbr_ubc = bytesToTransfer;
	xdmac_rx_cfg.mbr_da = (uint32_t)inBuffer;
	xdmac_rx_cfg.mbr_sa = (uint32_t)&(LINUX_SPI->SPI_RDR);
	xdmac_rx_cfg.mbr_cfg = XDMAC_CC_TYPE_PER_TRAN |
		XDMAC_CC_MBSIZE_SINGLE |
		XDMAC_CC_DSYNC_PER2MEM |
		XDMAC_CC_CSIZE_CHK_1 |
		XDMAC_CC_DWIDTH_BYTE|
		XDMAC_CC_SIF_AHB_IF1 |
		XDMAC_CC_DIF_AHB_IF0 |
		XDMAC_CC_SAM_FIXED_AM |
		XDMAC_CC_DAM_INCREMENTED_AM |
		XDMAC_CC_PERID(LINUX_XDMAC_RX_CH_NUM);
	xdmac_rx_cfg.mbr_bc = 0;
	xdmac_tx_cfg.mbr_ds = 0;
	xdmac_rx_cfg.mbr_sus = 0;
	xdmac_rx_cfg.mbr_dus = 0;
	xdmac_configure_transfer(XDMAC, DmacChanLinuxRx, &xdmac_rx_cfg);

	xdmac_channel_set_descriptor_control(XDMAC, DmacChanLinuxRx, 0);
	xdmac_channel_enable(XDMAC, DmacChanLinuxRx);
	xdmac_disable_interrupt(XDMAC, DmacChanLinuxRx);

	// Enable SPI and notify the RaspPi we are ready
	spi_enable(LINUX_SPI);

	// Enable end-of-transfer interrupt
	(void)LINUX_SPI->SPI_SR;						// clear any pending interrupt
	LINUX_SPI->SPI_IER = SPI_IER_NSSR;				// enable the NSS rising interrupt
	NVIC_SetPriority(LINUX_SPI_IRQn, NvicPrioritySpi);
	NVIC_EnableIRQ(LINUX_SPI_IRQn);

	// Begin transfer
	fastDigitalWriteHigh(LinuxTfrReadyPin);
}

void disable_spi()
{
	// Stop transfer
	fastDigitalWriteLow(LinuxTfrReadyPin);

	// Disable the XDMAC channels
	xdmac_channel_disable(XDMAC, DmacChanLinuxRx);
	xdmac_channel_disable(XDMAC, DmacChanLinuxTx);

	// Disable SPI
	spi_disable(LINUX_SPI);
}

extern "C" void LINUX_SPI_HANDLER(void)
{
	const uint32_t status = LINUX_SPI->SPI_SR;							// read status and clear interrupt
	LINUX_SPI->SPI_IDR = SPI_IER_NSSR;									// disable the interrupt
	if ((status & SPI_SR_NSSR) != 0)
	{
		// Data has been transferred, disable transfer ready pin and XDMAC channels
		disable_spi();
		dataReceived = true;

		// Check if any error occurred
		if ((status & SPI_SR_OVRES) != 0)
		{
			++spiRxOverruns;
		}
		if ((status & SPI_SR_UNDES) != 0)
		{
			++spiTxUnderruns;
		}
	}
}

/*-----------------------------------------------------------------------------------*/

DataTransfer::DataTransfer() : state(SpiState::ExchangingData), lastTransferTime(0), lastTransferNumber(0), failedTransfers(0),
	rxResponse(TransferResponse::Success), txResponse(TransferResponse::Success), rxPointer(0), txPointer(0), packetId(0)
{
	// Prepare RX header
	rxHeader.sequenceNumber = 0;

	// Prepare TX header
	txHeader.formatCode = LinuxFormatCode;
	txHeader.protocolVersion = LinuxProtocolVersion;
	txHeader.numPackets = 0;
	txHeader.sequenceNumber = 0;
}

void DataTransfer::Init() {
	// Initialise transfer ready pin
	pinMode(LinuxTfrReadyPin, OUTPUT_LOW);

	// Initialize SPI pins
	ConfigurePin(g_APinDescription[APIN_SPI1_MOSI]);
	ConfigurePin(g_APinDescription[APIN_SPI1_MISO]);
	ConfigurePin(g_APinDescription[APIN_SPI1_SCK]);
	ConfigurePin(g_APinDescription[APIN_SPI1_SS0]);

	// Initialise SPI
	spi_enable_clock(LINUX_SPI);
	spi_disable(LINUX_SPI);
	dataReceived = false;
}

void DataTransfer::Diagnostics(MessageType mtype)
{
	reprap.GetPlatform().MessageF(mtype, "State: %d, failed transfers: %u\n", (int)state, failedTransfers);
	reprap.GetPlatform().MessageF(mtype, "Last transfer: %" PRIu32 "ms ago\n", millis() - lastTransferTime);
	reprap.GetPlatform().MessageF(mtype, "RX/TX seq numbers: %d/%d\n", (int)rxHeader.sequenceNumber, (int)txHeader.sequenceNumber);
	reprap.GetPlatform().MessageF(mtype, "SPI underruns %u, overruns %u\n", spiTxUnderruns, spiRxOverruns);
}

const PacketHeader *DataTransfer::ReadPacket()
{
	if (rxPointer >= rxHeader.dataLength)
	{
		return nullptr;
	}

	const PacketHeader *header = reinterpret_cast<const PacketHeader*>(rxBuffer + rxPointer);
	rxPointer += sizeof(PacketHeader);
	return header;
}

const char *DataTransfer::ReadData(size_t dataLength)
{
	const char *data = rxBuffer + rxPointer;
	rxPointer += AddPadding(dataLength);
	return data;
}

template<typename T> const T *DataTransfer::ReadDataHeader()
{
	const T *header = reinterpret_cast<const T*>(rxBuffer + rxPointer);
	rxPointer += sizeof(T);
	return header;
}

uint8_t DataTransfer::ReadGetObjectModel()
{
	const ObjectModelHeader *header = ReadDataHeader<ObjectModelHeader>();
	return header->module;
}

void DataTransfer::ReadPrintStartedInfo(size_t packetLength, StringRef& filename, GCodeFileInfo& info)
{
	// Read header
	const PrintStartedHeader *header = ReadDataHeader<PrintStartedHeader>();
	info.numFilaments = header->numFilaments;
	info.lastModifiedTime = header->lastModifiedTime;
	info.fileSize = header->fileSize;
	info.firstLayerHeight = header->firstLayerHeight;
	info.layerHeight = header->layerHeight;
	info.objectHeight = header->objectHeight;
	info.printTime = header->printTime;
	info.simulatedTime = header->simulatedTime;

	// Read filaments
	memset(info.filamentNeeded, 0, ARRAY_SIZE(info.filamentNeeded) * sizeof(float));
	const char *data = ReadData(packetLength - sizeof(PrintStartedHeader));
	size_t filamentsSize = info.numFilaments * sizeof(float);
	memcpy(info.filamentNeeded, data, filamentsSize);
	data += filamentsSize;

	// Read file name
	filename.copy(data, header->filenameLength);
	data += header->filenameLength;

	// Read generated by
	info.generatedBy.copy(data, header->generatedByLength);
}

PrintStoppedReason DataTransfer::ReadPrintStoppedInfo()
{
	const PrintStoppedHeader *header = ReadDataHeader<PrintStoppedHeader>();
	return header->reason;
}

void DataTransfer::ReadMacroCompleteInfo(CodeChannel& channel, bool &error)
{
	const MacroCompleteHeader *header = ReadDataHeader<MacroCompleteHeader>();
	channel = header->channel;
	error = header->error;
}

void DataTransfer::ReadHeightMap()
{
	// Read heightmap header
	const HeightMapHeader *header = ReadDataHeader<HeightMapHeader>();
	float *xRange = new float[2] { header->xMin, header->xMax };
	float *yRange = new float[2] { header->yMin, header->yMax };
	float *spacing = new float[2] { header->xSpacing, header->ySpacing };
	reprap.GetGCodes().AssignGrid(xRange, yRange, header->radius, spacing);

	// Read Z coordinates
	const size_t numPoints = header->numX * header->numY;
	const float *points = reinterpret_cast<const float *>(ReadData(sizeof(float) * numPoints));

	HeightMap& map = reprap.GetMove().AccessHeightMap();
	map.ClearGridHeights();
	for (size_t i = 0; i < numPoints; i++)
	{
		if (!isnan(points[i]))
		{
			map.SetGridHeight(i, points[i]);
		}
	}

	// Activate it
	reprap.GetGCodes().ActivateHeightmap(true);
}

void DataTransfer::ReadLockUnlockRequest(CodeChannel& channel)
{
	const LockUnlockHeader *header = ReadDataHeader<LockUnlockHeader>();
	channel = header->channel;
}

void DataTransfer::ExchangeHeader()
{
	state = SpiState::ExchangingHeader;
	setup_spi(&rxHeader, &txHeader, sizeof(TransferHeader));
}

void DataTransfer::ExchangeResponse(uint32_t response)
{
	txResponse = response;
	state = (state == SpiState::ExchangingHeader) ? SpiState::ExchangingHeaderResponse : SpiState::ExchangingDataResponse;
	setup_spi(&rxResponse, &txResponse, sizeof(uint32_t));
}

void DataTransfer::ExchangeData()
{
	size_t bytesToExchange = max<size_t>(rxHeader.dataLength, txHeader.dataLength);
	state = SpiState::ExchangingData;
	setup_spi(rxBuffer, txBuffer, bytesToExchange);
}

void DataTransfer::ResetTransfer(bool ownRequest)
{
	if (reprap.Debug(moduleLinuxInterface))
	{
		reprap.GetPlatform().Message(DebugMessage, ownRequest ? "Resetting transfer\n" : "Resetting transfer due to Linux request\n");
	}
	failedTransfers++;

	if (ownRequest)
	{
		// Invalidate the data to send
		txResponse = TransferResponse::BadResponse;
		setup_spi(&rxResponse, &txResponse, sizeof(uint32_t));
		state = SpiState::Resetting;
	}
	else
	{
		// Linux wants to reset the state
		ExchangeHeader();
	}
}

bool DataTransfer::IsReady()
{
	if (dataReceived)
	{
		// Wait for the current XDMA transfer to finish. Relying on the XDMAC IRQ for this is does not work well...
		if ((xdmac_channel_get_status(XDMAC) & ((1 << DmacChanLinuxRx) | (1 << DmacChanLinuxTx))) != 0)
		{
			return false;
		}

		// Transfer has finished
		dataReceived = false;
		lastTransferTime = millis();

		switch (state)
		{
		case SpiState::ExchangingHeader:
		{
			// (1) Exchanged transfer headers
			const uint32_t headerResponse = *reinterpret_cast<const uint32_t*>(&rxHeader);
			if (headerResponse == TransferResponse::BadResponse)
			{
				// Linux wants to restart the transfer
				ResetTransfer(false);
				break;
			}

			const uint16_t checksum = CRC16(reinterpret_cast<const char *>(&rxHeader), sizeof(TransferHeader) - sizeof(uint16_t));
			if (rxHeader.checksumHeader != checksum)
			{
				if (reprap.Debug(moduleLinuxInterface))
				{
					reprap.GetPlatform().MessageF(DebugMessage, "Bad header checksum (expected %04" PRIx32 ", got %04" PRIx32 ")\n", (uint32_t)rxHeader.checksumHeader, (uint32_t)checksum);
				}
				ExchangeResponse(TransferResponse::BadHeaderChecksum);
				break;
			}

			if (rxHeader.formatCode != LinuxFormatCode)
			{
				ExchangeResponse(TransferResponse::BadFormat);
				break;
			}
			if (rxHeader.protocolVersion != LinuxProtocolVersion)
			{
				ExchangeResponse(TransferResponse::BadProtocolVersion);
				break;
			}
			if (rxHeader.dataLength > LinuxTransferBufferSize)
			{
				ExchangeResponse(TransferResponse::BadDataLength);
				break;
			}

			ExchangeResponse(TransferResponse::Success);
			break;
		}

		case SpiState::ExchangingHeaderResponse:
			// (2) Exchanged response to transfer header
			if (rxResponse == TransferResponse::Success && txResponse == TransferResponse::Success)
			{
				// Everything OK, perform the next data transfer
				if (rxHeader.dataLength != 0 || txHeader.dataLength != 0)
				{
					ExchangeData();
				}
				else
				{
					ExchangeHeader();
				}
			}
			else if (rxResponse == TransferResponse::BadResponse)
			{
				// Linux wants to restart the transfer
				ResetTransfer(false);
			}
			else if (rxResponse == TransferResponse::BadHeaderChecksum || txResponse == TransferResponse::BadHeaderChecksum)
			{
				// Failed to exchange header, restart the full transfer
				ExchangeHeader();
			}
			else
			{
				// Received invalid response code
				ResetTransfer(true);
			}
			break;

		case SpiState::ExchangingData:
		{
			// (3) Exchanged data
			if (rxBuffer32[0] == TransferResponse::BadResponse)
			{
				if (reprap.Debug(moduleLinuxInterface))
				{
					reprap.GetPlatform().Message(DebugMessage, "Resetting state due to Linux request\n");
				}
				ExchangeHeader();
				break;
			}

			const uint16_t checksum = CRC16(rxBuffer, rxHeader.dataLength);
			if (rxHeader.checksumData != checksum)
			{
				if (reprap.Debug(moduleLinuxInterface))
				{
					reprap.GetPlatform().MessageF(DebugMessage, "Bad data checksum (expected %04" PRIx32 ", got %04" PRIx32 ")\n", (uint32_t)rxHeader.checksumData, (uint32_t)checksum);
				}
				ExchangeResponse(TransferResponse::BadDataChecksum);
				break;
			}

			ExchangeResponse(TransferResponse::Success);
			break;
		}

		case SpiState::ExchangingDataResponse:
			// (4) Exchanged response to data transfer
			if (rxResponse == TransferResponse::Success && txResponse == TransferResponse::Success)
			{
				// Everything OK
				rxPointer = txPointer = 0;
				packetId = 0;
				state = SpiState::ProcessingData;
				return true;
			}

			if (rxResponse == TransferResponse::BadResponse)
			{
				// Linux wants to restart the transfer
				ResetTransfer(false);
			}
			else if (rxResponse == TransferResponse::BadDataChecksum || txResponse == TransferResponse::BadDataChecksum)
			{
				// Resend the data if a checksum error occurred
				ExchangeData();
			}
			else
			{
				// Received invalid response, reset the SPI transfer
				ResetTransfer(true);
			}
			break;

		case SpiState::Resetting:
			// Transmitted bad response, attempt to start a new transfer
			ExchangeHeader();
			break;

		default:
			// Should never get here. If we do, this probably means that StartNextTransfer has not been called
			ExchangeHeader();
			INTERNAL_ERROR;
			break;
		}
	}
	else if (state != SpiState::ExchangingHeader && millis() - lastTransferTime > SpiTransferTimeout)
	{
		// Reset failed transfers automatically after a certain period of time
		disable_spi();
		ExchangeHeader();
	}
	else if (!IsConnected())
	{
		// The Linux interface is no longer connected...
		rxHeader.sequenceNumber = 0;
	}
	return false;
}

void DataTransfer::StartNextTransfer()
{
	lastTransferNumber = rxHeader.sequenceNumber;

	// Reset RX transfer header
	rxHeader.formatCode = InvalidFormatCode;
	rxHeader.numPackets = 0;
	rxHeader.protocolVersion = 0;
	rxHeader.dataLength = 0;
	rxHeader.checksumData = 0;
	rxHeader.checksumHeader = 0;

	// Set up TX transfer header
	txHeader.numPackets = packetId;
	txHeader.sequenceNumber++;
	txHeader.dataLength = txPointer;
	txHeader.checksumData = CRC16(txBuffer, txPointer);
	txHeader.checksumHeader = CRC16(reinterpret_cast<const char *>(&txHeader), sizeof(TransferHeader) - sizeof(uint16_t));

	// Begin SPI transfer
	ExchangeHeader();
}

bool DataTransfer::WriteState(uint32_t busyChannels)
{
	if (!CanWritePacket(sizeof(ReportStateHeader)))
	{
		return false;
	}
	(void)WritePacketHeader(FirmwareRequest::ReportState, sizeof(ReportStateHeader));

	ReportStateHeader *state = WriteDataHeader<ReportStateHeader>();
	state->busyChannels = busyChannels;
	return true;
}

bool DataTransfer::WriteObjectModel(uint8_t module, OutputBuffer *data)
{
	// Try to write the packet header. This packet type cannot deal with truncated messages
	if (!CanWritePacket(data->Length()))
	{
		return false;
	}

	// Write packet header
	(void)WritePacketHeader(FirmwareRequest::ObjectModel, sizeof(ObjectModelHeader) + data->Length());

	// Write header
	ObjectModelHeader *header = WriteDataHeader<ObjectModelHeader>();
	header->length = data->Length();
	header->module = module;
	header->padding = 0;

	// Write data
	while (data != nullptr)
	{
		WriteData(data->UnreadData(), data->BytesLeft());
		data = OutputBuffer::Release(data);
	}
	return true;
}

bool DataTransfer::WriteCodeReply(MessageType type, OutputBuffer *&response)
{
	// Try to write the packet header. This packet type can deal with truncated messages
	if (!CanWritePacket(sizeof(CodeReplyHeader) + min<size_t>(24, response->Length())))
	{
		// Not enough space left
		return false;
	}
	PacketHeader *header = WritePacketHeader(FirmwareRequest::CodeReply);

	// Write header
	CodeReplyHeader *replyHeader = WriteDataHeader<CodeReplyHeader>();
	replyHeader->messageType = type;
	replyHeader->padding = 0;

	// Write code reply
	size_t bytesWritten = 0;
	if (response != nullptr)
	{
		do
		{
			size_t bytesToCopy = min<size_t>(LinuxTransferBufferSize - txPointer, response->BytesLeft());
			WriteData(response->UnreadData(), bytesToCopy);
			bytesWritten += bytesToCopy;

			response->Taken(bytesToCopy);
			if (response->BytesLeft() == 0)
			{
				response = OutputBuffer::Release(response);
			}
		}
		while (txPointer < LinuxTransferBufferSize && response != nullptr);

		if (response != nullptr)
		{
			// There is more data to come...
			replyHeader->messageType = (MessageType)(replyHeader->messageType | PushFlag);
		}
	}
	replyHeader->length = bytesWritten;

	// Finish packet and return what is left of the output buffer
	header->length = sizeof(CodeReplyHeader) + bytesWritten;
	return true;
}

bool DataTransfer::WriteMacroRequest(CodeChannel channel, const char *filename, bool reportMissing)
{
	size_t filenameLength = strlen(filename);
	if (!CanWritePacket(sizeof(ExecuteMacroHeader) + filenameLength))
	{
		return false;
	}

	// Write packet header
	WritePacketHeader(FirmwareRequest::ExecuteMacro, sizeof(ExecuteMacroHeader) + filenameLength);

	// Write header
	ExecuteMacroHeader *header = WriteDataHeader<ExecuteMacroHeader>();
	header->channel = channel;
	header->reportMissing = reportMissing;
	header->length = filenameLength;
	header->padding = 0;

	// Write filename
	WriteData(filename, filenameLength);
	return true;
}

bool DataTransfer::WriteAbortFileRequest(CodeChannel channel)
{
	if (!CanWritePacket(sizeof(AbortFileHeader)))
	{
		return false;
	}

	// Write packet header
	WritePacketHeader(FirmwareRequest::AbortFile, sizeof(AbortFileHeader));

	// Write header
	AbortFileHeader *header = WriteDataHeader<AbortFileHeader>();
	header->channel = channel;
	header->paddingA = 0;
	header->paddingB = 0;
	return true;
}

bool DataTransfer::WriteStackEvent(CodeChannel channel, GCodeMachineState& state)
{
	if (!CanWritePacket(sizeof(StackEventHeader)))
	{
		return false;
	}

	// Get stack depth
	uint8_t stackDepth = 0;
	for (GCodeMachineState *ms = &state; ms != nullptr; ms = ms->previous)
	{
		stackDepth++;
	}

	// Write packet header
	WritePacketHeader(FirmwareRequest::StackEvent, sizeof(StackEventHeader));

	// Write header
	StackEventHeader *header = WriteDataHeader<StackEventHeader>();
	header->channel = channel;
	header->depth = stackDepth;
	header->flags = StackEventFlags::none;
	if (state.axesRelative)
	{
		header->flags = (StackEventFlags)(header->flags | StackEventFlags::axesRelative);
	}
	if (state.drivesRelative)
	{
		header->flags = (StackEventFlags)(header->flags | StackEventFlags::drivesRelative);
	}
	if (state.volumetricExtrusion)
	{
		header->flags = (StackEventFlags)(header->flags | StackEventFlags::volumetricExtrusion);
	}
	if (state.usingInches)
	{
		header->flags = (StackEventFlags)(header->flags | StackEventFlags::usingInches);
	}
	header->feedrate = state.feedRate;
	return true;
}

bool DataTransfer::WritePrintPaused(FilePosition position, PrintPausedReason reason)
{
	if (!CanWritePacket(sizeof(PrintPausedHeader)))
	{
		return false;
	}

	// Write packet header
	WritePacketHeader(FirmwareRequest::PrintPaused, sizeof(PrintPausedHeader));

	// Write header
	PrintPausedHeader *header = WriteDataHeader<PrintPausedHeader>();
	header->filePosition = position;
	header->pauseReason = reason;
	header->paddingA = 0;
	header->paddingB = 0;
	return true;
}

bool DataTransfer::WriteHeightMap()
{
	const GridDefinition& grid = reprap.GetMove().GetGrid();
	size_t numPoints = reprap.GetMove().AccessHeightMap().UsingHeightMap() ? grid.NumPoints() : 0;
	size_t bytesToWrite = sizeof(HeightMapHeader) + numPoints * sizeof(float);
	if (!CanWritePacket(bytesToWrite))
	{
		return false;
	}

	// Write packet header
	(void)WritePacketHeader(FirmwareRequest::HeightMap, bytesToWrite);

	// Write heightmap header
	HeightMapHeader *header = WriteDataHeader<HeightMapHeader>();
	header->xMin = grid.xMin;
	header->xMax = grid.xMax;
	header->xSpacing = grid.xSpacing;
	header->yMin = grid.yMin;
	header->yMax = grid.yMax;
	header->ySpacing = grid.ySpacing;
	header->radius = grid.radius;
	header->numX = grid.numX;
	header->numY = grid.numY;

	// Write Z points
	if (numPoints != 0)
	{
		float *zPoints = reinterpret_cast<float*>(txBuffer + txPointer);
		reprap.GetMove().SaveHeightMapToArray(zPoints);
		txPointer += numPoints * sizeof(float);
	}
	return true;
}

bool DataTransfer::WriteLocked(CodeChannel channel)
{
	if (!CanWritePacket(sizeof(LockUnlockHeader)))
	{
		return false;
	}

	// Write packet header
	WritePacketHeader(FirmwareRequest::Locked, sizeof(LockUnlockHeader));

	// Write header
	LockUnlockHeader *header = WriteDataHeader<LockUnlockHeader>();
	header->channel = channel;
	header->paddingA = 0;
	header->paddingB = 0;
	return true;
}

PacketHeader *DataTransfer::WritePacketHeader(FirmwareRequest request, size_t dataLength, uint16_t resendPacketId)
{
	// Make sure to stay aligned if the last packet ended with a string
	txPointer = AddPadding(txPointer);

	// Write the next packet data
	PacketHeader *header = reinterpret_cast<PacketHeader*>(txBuffer + txPointer);
	header->request = static_cast<uint16_t>(request);
	header->id = packetId++;
	header->length = dataLength;
	header->resendPacketId = resendPacketId;
	txPointer += sizeof(PacketHeader);
	return header;
}

void DataTransfer::WriteData(const char *data, size_t length)
{
	// Strings can be concatenated here, don't add any padding yet
	memcpy(txBuffer + txPointer, data, length);
	txPointer += length;
}

template<typename T> T *DataTransfer::WriteDataHeader()
{
	T *header = reinterpret_cast<T*>(txBuffer + txPointer);
	txPointer += sizeof(T);
	return header;
}

uint16_t DataTransfer::CRC16(const char *buffer, size_t length) const
{
	const uint16_t crc16_table[] = {
        0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
        0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
        0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
        0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
        0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
        0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
        0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
        0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
        0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
        0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
        0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
        0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
        0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
        0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
        0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
        0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
        0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
        0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
        0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
        0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
        0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
        0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
        0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
        0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
        0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
        0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
        0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
        0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
        0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
        0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
        0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
        0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
    };

    uint16_t Crc = 65535;
    uint16_t x;
    for (size_t i = 0; i < length; i++)
    {
        x = (uint16_t)(Crc ^ buffer[i]);
        Crc = (uint16_t)((Crc >> 8) ^ crc16_table[x & 0x00FF]);
    }

    return Crc;
}

#endif
