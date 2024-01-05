// VfdrawcmdSys driver alias virtual fdrawcmd driver.

#include "VfdrawcmdSys.h"
#include "BitstreamTrackBuilder.h"
#include "IBMPCBase.h"
#include "MemFile.h"
#include "Options.h"
#include "SpecialFormat.h"
#include "win32_error.h"

#include "Cpp_helpers.h"
#include "utils.h"

#include <algorithm>

static auto& opt_debug = getOpt<int>("debug");

/*static*/ std::unique_ptr<VfdrawcmdSys> VfdrawcmdSys::Open(const std::string& path) // int device_index
{
    if (IsDir(path))
        return std::make_unique<VfdrawcmdSys>(path);

    return std::unique_ptr<VfdrawcmdSys>();
}

const uint32_t VfdrawcmdSys::DEFAULT_TRACKTIMES[4]{200000, 166666, 200000, 200000}; // Tracktimes for FDRates.

VfdrawcmdSys::VfdrawcmdSys(const std::string& path)
    : FdrawcmdSys(INVALID_HANDLE_VALUE)
{
    m_path = path;
}

util::Version& VfdrawcmdSys::GetVersion()
{
    if (m_driver_version.value == 0)
        if (!GetVersion(m_driver_version))
            throw win32_error(GetLastError_MP(), "GetVersion");
    return m_driver_version;
}

FD_FDC_INFO* VfdrawcmdSys::GetFdcInfo()
{
    if (!m_fdc_info_queried)
    {
        if (!GetFdcInfo(m_fdc_info))
            return nullptr;
        m_fdc_info_queried = true;
    }
    return &m_fdc_info;
}

int VfdrawcmdSys::GetMaxTransferSize()
{
    return MAX_TRANSFER_SIZE;
}

////////////////////////////////////////////////////////////////////////////////

bool VfdrawcmdSys::GetVersion(util::Version& version)
{
    version.value = DriverVersion1_0_1_12;
    return true;
}

bool VfdrawcmdSys::GetResult(FD_CMD_RESULT& result)
{
    result = m_result;
    return true;
}

// Returning false means there are no sectors thus can not wait.
bool VfdrawcmdSys::AdvanceSectorIndexByFindingSectorIds(const OrphanDataCapableTrack& orphanDataCapableTrack, uint8_t count/* = 1*/, bool* looped/* = nullptr*/)
{
    bool loopedLocal;
    if (looped == nullptr)
        looped = &loopedLocal;
    *looped = false;
    if (count == 0)
        return true;
    const auto trackSectorCount = orphanDataCapableTrack.track.size();
    if (trackSectorCount <= 0)
        return false;
    const auto trackLen = orphanDataCapableTrack.getOffsetOfTime(static_cast<int>(m_trackTime));
    while ((static_cast<int>(m_currentSectorIndex) + count) >= trackSectorCount)
    {
        count -= trackSectorCount - (static_cast<int>(m_currentSectorIndex));
        const auto itlastSector = orphanDataCapableTrack.track.end() - 1;
        auto offset = itlastSector->offset + GetSectorOverhead(fdEncodingToEncoding(m_encoding_flags)) * 16; // Addition is not perfect but good enough now. mfmbits
        if (offset < trackLen)
            m_currentSectorIndex = 0;
        else
        {
            do
            {
                offset -= trackLen;
            } while (offset >= trackLen);
            const auto it = orphanDataCapableTrack.track.findFirstFromOffset(offset);
            m_currentSectorIndex = lossless_static_cast<uint8_t>(it - orphanDataCapableTrack.track.begin());
        }
        *looped = true;
    }
    m_currentSectorIndex = static_cast<uint8_t>(static_cast<int>(m_currentSectorIndex) + count);
    return true;
}

void VfdrawcmdSys::WaitIndex(int head/* = -1*/, const bool calcSpinTime/* = false*/)
{
    m_currentSectorIndex = 0; // alias WaitIndex in fdrawcmd.
    if (calcSpinTime)
    {
        const auto& orphanDataCapableTrack = ReadTrackFromRowTrack(CylHead(m_cyl, lossless_static_cast<uint8_t>(head)));
        const auto bestTrackTime = !orphanDataCapableTrack.track.empty() && m_trackTime > 0 ?
                   orphanDataCapableTrack.determineBestTrackTime(static_cast<int>(m_trackTime)) : 0;
        m_trackTime = bestTrackTime > 0 ? static_cast<uint32_t>(bestTrackTime): DEFAULT_TRACKTIMES[m_fdrate];
    }
}

// Returning false means there are no sectors thus can not wait.
bool VfdrawcmdSys::WaitSector(const OrphanDataCapableTrack& orphanDataCapableTrack)
{
    if (!m_waitSector)
        return true;
    m_waitSector = false;

    WaitIndex();

    return AdvanceSectorIndexByFindingSectorIds(orphanDataCapableTrack, m_waitSectorCount);
}

bool VfdrawcmdSys::SetPerpendicularMode(int /*ow_ds_gap_wgate*/)
{
    return true;
}

void VfdrawcmdSys::LimitCyl()
{
    if (m_cyl < 0)
        m_cyl = 0;
    if (m_cyl > 82)
        m_cyl = 82;
}

const RawTrackMFM& VfdrawcmdSys::ReadRawTrack(const CylHead& cylhead)
{
    if (!m_rawTrackLoaded[lossless_static_cast<size_t>(cylhead.operator int())])
    {
        MemFile file;
        RawTrackMFM rawTrackMFM;
        const auto pattern = " Raw track (track %02d, head %1d).floppy_raw_track";
        const auto fileNamePart = util::fmt(pattern, cylhead.cyl, cylhead.head);
        const auto rawTrackFilePath = FindFirstFile(fileNamePart, m_path);

        do
        {
            if (rawTrackFilePath.empty())
                break;
            try
            {
                file.open(rawTrackFilePath);
            }
            catch (...)
            {
                break;
            }
            std::vector<uint8_t> rawTrackContent(lossless_static_cast<std::vector<uint8_t>::size_type>(file.size()));
            if (file.rewind() && file.read(rawTrackContent))
                rawTrackMFM = RawTrackMFM(file.data(), FDRATE_TO_DATARATE[m_fdrate]);
        } while (false);
        m_rawTracks[cylhead] = std::move(rawTrackMFM);
        m_rawTrackLoaded[lossless_static_cast<size_t>(cylhead.operator int())] = true;
    }
    return m_rawTracks[cylhead];
}

OrphanDataCapableTrack VfdrawcmdSys::ReadTrackFromRowTrack(const CylHead& cylhead)
{
    if (!m_odcTrackDecoded[lossless_static_cast<size_t>(cylhead.operator int())])
    {
        const auto& rawTrack = ReadRawTrack(cylhead);
        const auto orphanDataCapableTrack = rawTrack.DecodeTrack(cylhead);
        m_odcTracks[cylhead] = std::move(orphanDataCapableTrack);
        m_odcTrackDecoded[lossless_static_cast<size_t>(cylhead.operator int())] = true;
    }
    return m_odcTracks[cylhead];
}

bool VfdrawcmdSys::SetEncRate(Encoding encoding, DataRate datarate)
{
    const auto fdrate = datarateToFdRate(datarate);
    m_encoding_flags = encodingToFdEncoding(encoding);
    m_fdrate = fdrate;

    // Set perpendicular mode and write-enable for 1M data rate
    SetPerpendicularMode((datarate == DataRate::_1M) ? 0xbc : 0x00);

    return true;
}

bool VfdrawcmdSys::SetHeadSettleTime(int /*ms*/)
{
    return true;
}

bool VfdrawcmdSys::SetMotorTimeout(int /*seconds*/)
{
    return true;
}

bool VfdrawcmdSys::SetMotorOff()
{
    return true;
}

bool VfdrawcmdSys::SetDiskCheck(bool /*enable*/)
{
    return true;
}

bool VfdrawcmdSys::GetFdcInfo(FD_FDC_INFO& info)
{
    info.SpeedsAvailable = FDC_SPEED_250K;
    info.MaxTransferSize = MAX_TRANSFER_SIZE;
    return true;
}

bool VfdrawcmdSys::CmdPartId(uint8_t& part_id)
{
    part_id = 128; // This value is found by experiment.
    return true;
}

bool VfdrawcmdSys::Configure(uint8_t /*eis_efifo_poll_fifothr*/, uint8_t /*pretrk*/)
{
    return true;
}

bool VfdrawcmdSys::Specify(int /*step_rate*/, int /*head_unload_time*/, int /*head_load_time*/)
{
    // step_rate is a coded number between 0 and 15. Not supporting it here.
    return true;
}

bool VfdrawcmdSys::Recalibrate()
{
    return true;
}

bool VfdrawcmdSys::Seek(int cyl, int /*head*//*= -1*/)
{
    m_cyl = cyl;
    LimitCyl();
    return true;
}

bool VfdrawcmdSys::RelativeSeek(int /*head*/, int offset)
{
    m_cyl += offset;
    LimitCyl();
    return true;
}

bool VfdrawcmdSys::CmdVerify(int cyl, int head, int sector, int size, int eot)
{
    return CmdVerify(head, cyl, head, sector, size, eot);
}

bool VfdrawcmdSys::CmdVerify(int /*phead*/, int cyl, int head, int sector, int size, int /*eot*/)
{
//    if (!WaitSector(orphanDataCapableTrack)) ;
    // Must set result.
    m_result.cyl = lossless_static_cast<uint8_t>(cyl);
    m_result.head = lossless_static_cast<uint8_t>(head);
    m_result.sector = lossless_static_cast<uint8_t>(sector);
    m_result.size = lossless_static_cast<uint8_t>(size);
    m_result.st0 = 0;
    m_result.st1 = STREG1_MISSING_ADDRESS_MARK;
    m_result.st2 = 0;
    SetLastError_MP(ERROR_NOT_SUPPORTED);
    return false; // Not implemented so return failure.
}

/*
 * Input:
 * - eot: number of sectors
 * - mem: the memory where the read sectors are stored. Its size is auto calculated if 0.
 */
bool VfdrawcmdSys::CmdReadTrack(int phead, int cyl, int /*head*/, int /*sector*/, int size, int eot, MEMORY& mem)
{
    const auto sizeChecked = lossless_static_cast<uint8_t>(size);
    const auto eotChecked = lossless_static_cast<uint8_t>(eot);

    const auto sector_size = Sector::SizeCodeToRealLength(sizeChecked);
    auto output_size = std::min(eotChecked * sector_size, (GetMaxTransferSize() / sector_size + 1) * sector_size);
    if (mem.size > 0)
    {
        if (mem.size < output_size)
        {
            mem.resize((mem.size / sector_size) * sector_size);
            output_size = mem.size;
        }
        else
            mem.fill();
    }
    else
        mem.resize(output_size);
    // mem.size >= output_size now.

    const auto currentRawTrack = ReadRawTrack(CylHead(lossless_static_cast<uint8_t>(cyl), lossless_static_cast<uint8_t>(phead)));
    const auto availSize = std::min(lossless_static_cast<int>(currentRawTrack.m_rawTrackContent.Bytes().size()), output_size);
    mem.copyFrom(currentRawTrack.m_rawTrackContent.Bytes(), availSize);
    return true;
}

bool VfdrawcmdSys::CmdRead(int phead, int cyl, int head, int sector, int size, int count, MEMORY& mem, size_t data_offset, bool deleted)
{
    if (count == 0)
    {
        SetLastError_MP(STATUS_INVALID_PARAMETER);
        return false;
    }
    const auto sectorSize = Sector::SizeCodeToRealLength(static_cast<uint8_t>(size));
    const auto totalSize = count * sectorSize;
    auto i_dataOffset = lossless_static_cast<int>(data_offset);
    if (mem.size - i_dataOffset > totalSize)
    {
        SetLastError_MP(STATUS_BUFFER_TOO_SMALL);
        return false;
    }

    // Must set result.
    m_result.st0 = 0;
    m_result.st1 = 0;
    m_result.st2 = 0;
    if (m_encoding_flags == FD_OPTION_MFM && m_fdrate == FD_RATE_250K)
    {
        const auto& orphanDataCapableTrack = ReadTrackFromRowTrack(CylHead(m_cyl, lossless_static_cast<uint8_t>(phead)));
        if (WaitSector(orphanDataCapableTrack) && !orphanDataCapableTrack.track.empty())
        {
            const auto sectorIndexStart = m_currentSectorIndex;
            auto loopedOnceAtLeast = false;
            do
            {
                const auto& sectorCurrent = orphanDataCapableTrack.track[m_currentSectorIndex];
                bool looped;
                if (AdvanceSectorIndexByFindingSectorIds(orphanDataCapableTrack, 1, &looped) && looped)
                    loopedOnceAtLeast = true;
                if (sectorCurrent.header.cyl == cyl && sectorCurrent.header.head == head
                        && sectorCurrent.header.sector == sector && sectorCurrent.header.size == size
                        && sectorCurrent.dam == (deleted ? IBM_DAM_DELETED : IBM_DAM))
                {
                    m_result.cyl = lossless_static_cast<uint8_t>(sectorCurrent.header.cyl);
                    m_result.head = lossless_static_cast<uint8_t>(sectorCurrent.header.head);
                    m_result.sector = lossless_static_cast<uint8_t>(sectorCurrent.header.sector);
                    m_result.size = lossless_static_cast<uint8_t>(sectorCurrent.header.size);
                    if (!sectorCurrent.has_data())
                    {
                        m_result.st1 = STREG1_NO_DATA;
                        SetLastError_MP(ERROR_SECTOR_NOT_FOUND);
                        return false;
                    }
                    mem.copyFrom(sectorCurrent.data_copy(), sectorSize, i_dataOffset);
                    i_dataOffset += sectorSize;
                    count--;
                }
            } while(count > 0 && (!loopedOnceAtLeast || m_currentSectorIndex < sectorIndexStart));
        }
    }
    if (count == 0)
        return true;
    m_result.st1 = STREG1_MISSING_ADDRESS_MARK;
    SetLastError_MP(ERROR_FLOPPY_ID_MARK_NOT_FOUND);
    return false;
}

bool VfdrawcmdSys::CmdWrite(int /*phead*/, int cyl, int head, int sector, int size, int /*count*/, MEMORY& /*mem*/, bool /*deleted*/)
{
//    if (!WaitSector(orphanDataCapableTrack)) ;
    // Must set result.
    m_result.cyl = lossless_static_cast<uint8_t>(cyl);
    m_result.head = lossless_static_cast<uint8_t>(head);
    m_result.sector = lossless_static_cast<uint8_t>(sector);
    m_result.size = lossless_static_cast<uint8_t>(size);
    m_result.st0 = 0;
    m_result.st1 = STREG1_MISSING_ADDRESS_MARK;
    m_result.st2 = 0;
    SetLastError_MP(ERROR_NOT_SUPPORTED);
    return false; // Not implemented so return failure.
}

bool VfdrawcmdSys::CmdFormat(FD_FORMAT_PARAMS* /*params*/, int /*size*/)
{
    SetLastError_MP(ERROR_NOT_SUPPORTED);
    return false; // Not implemented so return failure.
}

bool VfdrawcmdSys::CmdFormatAndWrite(FD_FORMAT_PARAMS* /*params*/, int /*size*/)
{
    SetLastError_MP(ERROR_NOT_SUPPORTED);
    return false; // Not implemented so return failure.
}

bool VfdrawcmdSys::CmdScan(int head, FD_SCAN_RESULT* scan, int size)
{
    const auto sectors = (lossless_static_cast<size_t>(size) - sizeof(FD_SCAN_RESULT)) / sizeof(FD_ID_HEADER);
    if (sectors == 0)
    {
        SetLastError_MP(ERROR_NOT_ENOUGH_MEMORY);
        return false; // The scan can not hold the found headers.
    }

    // WaitIndex(head, true); // Currently calling it in the called CmdTimedScan method.

    const auto timedScanSize = lossless_static_cast<int>(sizeof(FD_TIMED_SCAN_RESULT) + sizeof(FD_TIMED_ID_HEADER) * sectors);
    const MEMORY mem(timedScanSize);
    const auto timedScanResult = reinterpret_cast<FD_TIMED_SCAN_RESULT*>(mem.pb);

    const auto result = CmdTimedScan(head, timedScanResult, timedScanSize);
    if (!result)
        return result;

    scan->count = timedScanResult->count;
    for (int i = 0; i < timedScanResult->count; i++)
    {
        scan->HeaderArray(i).cyl = timedScanResult->HeaderArray(i).cyl;
        scan->HeaderArray(i).head = timedScanResult->HeaderArray(i).head;
        scan->HeaderArray(i).sector = timedScanResult->HeaderArray(i).sector;
        scan->HeaderArray(i).size = timedScanResult->HeaderArray(i).size;
    }

    return true;
}

bool VfdrawcmdSys::CmdTimedScan(int head, FD_TIMED_SCAN_RESULT* timed_scan, int size)
{
    const auto sectors = (lossless_static_cast<size_t>(size) - sizeof(FD_TIMED_SCAN_RESULT)) / sizeof(FD_TIMED_ID_HEADER);
    if (sectors == 0)
    {
        SetLastError_MP(ERROR_NOT_ENOUGH_MEMORY);
        return false; // The timed_scan can not hold the found headers.
    }

    // WaitIndex(head, true); // Currently calling it in the called CmdTimedMultiScan method.

    const auto timedMultiScanSize = lossless_static_cast<int>(sizeof(FD_TIMED_MULTI_SCAN_RESULT) + sizeof(FD_TIMED_MULTI_ID_HEADER) * sectors);
    const MEMORY mem(timedMultiScanSize);
    const auto timedMultiScanResult = reinterpret_cast<FD_TIMED_MULTI_SCAN_RESULT*>(mem.pb);

    const auto result = CmdTimedMultiScan(head, 1, timedMultiScanResult, timedMultiScanSize); // TODO track_retries = 1 is disturbing
    if (!result)
        return result;

    timed_scan->count = lossless_static_cast<BYTE>(timedMultiScanResult->count);
    for (int i = 0; i < timedMultiScanResult->count; i++)
    {
        timed_scan->HeaderArray(i).cyl = timedMultiScanResult->HeaderArray(i).cyl;
        timed_scan->HeaderArray(i).head = timedMultiScanResult->HeaderArray(i).head;
        timed_scan->HeaderArray(i).sector = timedMultiScanResult->HeaderArray(i).sector;
        timed_scan->HeaderArray(i).size = timedMultiScanResult->HeaderArray(i).size;
        timed_scan->HeaderArray(i).reltime = timedMultiScanResult->HeaderArray(i).reltime;
    }
    timed_scan->firstseen = 0; // Where the first sector was seen?
    timed_scan->tracktime = timedMultiScanResult->tracktime;

    return true;
}

bool VfdrawcmdSys::CmdTimedMultiScan(int head, int track_retries,
                                    FD_TIMED_MULTI_SCAN_RESULT* timed_multi_scan, int size,
                                    int byte_tolerance_of_time /* = -1 */)
{
    if (head < 0 || head > 1)
        throw util::exception("unsupported head (", head, ")");
    if (track_retries == 0)
        throw util::exception("unsupported track_retries (", track_retries, ")");

    const auto sectors = static_cast<int>((lossless_static_cast<size_t>(size) - sizeof(FD_TIMED_MULTI_SCAN_RESULT)) / sizeof(FD_TIMED_MULTI_ID_HEADER));
    if (sectors == 0)
    {
        SetLastError_MP(ERROR_NOT_ENOUGH_MEMORY);
        return false; // The timed_multi_scan can not hold the found headers.
    }

    WaitIndex(head, true);

    timed_multi_scan->byte_tolerance_of_time = byte_tolerance_of_time < 0 ? Track::COMPARE_TOLERANCE_BYTES : lossless_static_cast<uint8_t>(byte_tolerance_of_time);
    timed_multi_scan->tracktime = m_trackTime;
    timed_multi_scan->track_retries = lossless_static_cast<uint8_t>(std::abs(track_retries));

    const CylHead cylHead{m_cyl, lossless_static_cast<uint8_t>(head)};
    const auto& orphanDataCapableTrack = ReadTrackFromRowTrack(cylHead);
    timed_multi_scan->count = lossless_static_cast<WORD>(orphanDataCapableTrack.track.size());
    if (timed_multi_scan->count > 0)
    {
        const auto trackTime = static_cast<int>(timed_multi_scan->tracktime);
        Track trackTemp;
        trackTemp.tracktime = trackTime;
        trackTemp.tracklen = orphanDataCapableTrack.getOffsetOfTime(trackTime);

        // Demulti the temporary track so we can check how many sectors it has.
        const auto iSup = lossless_static_cast<int>(timed_multi_scan->count);
        for (int i = 0; i < iSup; i++)
        {
            auto sector = Sector(orphanDataCapableTrack.track[i]);
            const auto rawTimeOffseted = orphanDataCapableTrack.getTimeOfOffset(sector.offset);
            sector.offset = rawTimeOffseted % trackTime; // Demultid offset.
            trackTemp.add(std::move(sector));
        }

        const auto short_mfm_gap = Is11SectorTrack(trackTemp);
        BitstreamTrackBuilder bitbuf(orphanDataCapableTrack.track[0].datarate, orphanDataCapableTrack.track[0].encoding);
        bitbuf.addTrackStart(short_mfm_gap); // Depends on if track is special with 11 sectors MFM 250 Kbps.

        // Finding track index offset.
        int trackIndexOffset = orphanDataCapableTrack.trackIndexOffset;
        if (trackIndexOffset == 0)
        { // Try to sync to sector ID=1 because usually it exists (though not always the first sector, see interleaving).
            const auto it = orphanDataCapableTrack.track.findIgnoringSize(Header(cylHead, 1, 0));
            if (it == orphanDataCapableTrack.track.end())
            {
                if (opt_debug)
                    util::cout << "CmdTimedMultiScan found neither IAM nor sector ID=1 thus ignoring raw track without start sync\n";
                timed_multi_scan->count = 0;
                return true;
            }
            else
                trackIndexOffset = it->offset - bitbuf.gapPreIDAMBits(short_mfm_gap);
        }

        const auto trackStartOffset = trackIndexOffset - bitbuf.getIAMPosition(); // Syncs so the track start matches here and in BitstreamTrackBuilder.

        // Demulti the temporary track so we can produce the result array adding each unique sector exactly once.
        trackTemp.clear();
        int j = 0;
        for (int i = 0; i < iSup; i++)
        {
            auto sector = Sector(orphanDataCapableTrack.track[i]);
            const auto rawTimeOffseted = orphanDataCapableTrack.getTimeOfOffset(sector.offset - trackStartOffset + trackTemp.tracklen);
            sector.offset = rawTimeOffseted % trackTime; // Demultid offset.
            const auto addResult = trackTemp.add(std::move(sector));
            if (addResult == Track::AddResult::Append || addResult == Track::AddResult::Insert)
            {
                if (j == sectors)
                {
                    SetLastError_MP(ERROR_NOT_ENOUGH_MEMORY);
                    return false; // The timed_multi_scan can not hold the found headers.
                }
                timed_multi_scan->HeaderArray(j).cyl = lossless_static_cast<BYTE>(sector.header.cyl);
                timed_multi_scan->HeaderArray(j).head = lossless_static_cast<BYTE>(sector.header.head);
                timed_multi_scan->HeaderArray(j).sector = lossless_static_cast<BYTE>(sector.header.sector);
                timed_multi_scan->HeaderArray(j).size = lossless_static_cast<BYTE>(sector.header.size);
                timed_multi_scan->HeaderArray(j).reltime = lossless_static_cast<DWORD>(sector.offset);
                timed_multi_scan->HeaderArray(j).revolution = lossless_static_cast<BYTE>(orphanDataCapableTrack.getTimeOfOffset(sector.offset) / trackTime);
                j++;
            }
        }
        timed_multi_scan->count = lossless_static_cast<WORD>(j);

        // The timed_multi_scan must be sorted. The PFD_TIMED_MULTI_ID_HEADER_EXT type is used which
        // is same data as PFD_TIMED_MULTI_ID_HEADER. The comparison operators' code is taken from
        // MultiScan.h of fdrawcmd project.
        std::sort(static_cast<PFD_TIMED_MULTI_ID_HEADER_EXT>(&timed_multi_scan->HeaderArray(0)),
                  static_cast<PFD_TIMED_MULTI_ID_HEADER_EXT>(&timed_multi_scan->HeaderArray(j)),
                  [](const FD_TIMED_MULTI_ID_HEADER_EXT& lhs, const FD_TIMED_MULTI_ID_HEADER_EXT& rhs) {
            return lhs < rhs;
        });
    }
    return true;
}

bool VfdrawcmdSys::CmdReadId(int head, FD_CMD_RESULT& result)
{
    // Must set result.
    m_result.st0 = 0;
    m_result.st1 = 0;
    m_result.st2 = 0;
    bool foundId = false;
    if (m_encoding_flags == FD_OPTION_MFM && m_fdrate == FD_RATE_250K)
    {
        const auto& orphanDataCapableTrack = ReadTrackFromRowTrack(CylHead(m_cyl, lossless_static_cast<uint8_t>(head)));
        if (WaitSector(orphanDataCapableTrack) && !orphanDataCapableTrack.track.empty())
        {
            const auto& sectorCurrent = orphanDataCapableTrack.track[m_currentSectorIndex];
            AdvanceSectorIndexByFindingSectorIds(orphanDataCapableTrack);
            m_result.cyl = lossless_static_cast<uint8_t>(sectorCurrent.header.cyl);
            m_result.head = lossless_static_cast<uint8_t>(sectorCurrent.header.head);
            m_result.sector = lossless_static_cast<uint8_t>(sectorCurrent.header.sector);
            m_result.size = lossless_static_cast<uint8_t>(sectorCurrent.header.size);
            foundId = true;
        }
    }
    if (!foundId)
    {
        m_result.st1 = STREG1_MISSING_ADDRESS_MARK;
        SetLastError_MP(ERROR_FLOPPY_ID_MARK_NOT_FOUND);
    }
    result = m_result;
    return foundId;
}

bool VfdrawcmdSys::FdRawReadTrack(int /*head*/, int /*size*/, MEMORY& /*mem*/)
{
    SetLastError_MP(ERROR_NOT_SUPPORTED);
    return false; // Not implemented so return failure.
}

bool VfdrawcmdSys::FdSetSectorOffset(int index)
{
    m_waitSectorCount = limited_static_cast<uint8_t>(index);
    m_waitSector = true;
    return true;

}

bool VfdrawcmdSys::FdSetShortWrite(int /*length*/, int /*finetune*/)
{
    SetLastError_MP(ERROR_NOT_SUPPORTED);
    return false; // Not implemented so return failure.
}

bool VfdrawcmdSys::FdGetRemainCount(int& /*remain*/)
{
    SetLastError_MP(ERROR_NOT_SUPPORTED);
    return false; // Not implemented so return failure.
}

bool VfdrawcmdSys::FdCheckDisk()
{
    return true;
}

bool VfdrawcmdSys::FdGetTrackTime(int& microseconds)
{
    WaitIndex(0, true);

    microseconds = static_cast<int>(m_trackTime);
    return true;
}

bool VfdrawcmdSys::FdGetMultiTrackTime(FD_MULTI_TRACK_TIME_RESULT& track_time, uint8_t /*revolutions*//* = 10*/)
{
    WaitIndex(0, true);

    track_time.spintime = m_trackTime;
    return true;
}

bool VfdrawcmdSys::FdReset()
{
    return true;
}