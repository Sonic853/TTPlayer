#include "ttplayer/audio/disc_lookup.h"
#include "ttplayer/core/text.h"
#include "ttplayer/net/http.h"
#include "ttplayer/build_date.h"
#include "ttplayer/platform/optional_windows_api.h"
#include "../update/json.h"
#include <algorithm>
#include <mutex>
#include <thread>
#include <winhttp.h>
#include <mfapi.h>

namespace ttplayer::audio {
namespace {
using update::detail::Json;
using Clock=std::chrono::steady_clock;
void Guard(const DiscCancel& canceled){if(canceled&&canceled())throw std::runtime_error("Canceled");}
bool Mbid(std::string_view id) {
    if(id.size()!=36)return false;
    for(size_t i=0;i<id.size();++i)if(i==8||i==13||i==18||i==23){if(id[i]!='-')return false;}
    else if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return false;
    return true;
}
std::wstring Wide(const Json& value) {auto text=value.String();if(text.find('\0')!=text.npos)throw std::runtime_error("Invalid metadata text");return core::Utf8ToWide(text);}
std::wstring Credit(const Json& credit) {
    std::wstring result;
    for(const auto& item:credit.items) {
        if(item.kind==Json::string){result+=Wide(item);continue;}
        auto name=Wide(item["name"]);if(name.empty())name=Wide(item["artist"]["name"]);
        result+=name+Wide(item["joinphrase"]);
    }
    return result;
}
void Add(CueMetadata& fields,const wchar_t* name,const std::wstring& value){if(!value.empty())fields.emplace_back(name,value);}
std::wstring Base(const settings::NetworkSettings& network) {
    auto base=network.freedb_server;
    if(base.empty() || base.find(L"freedb.")!=base.npos || base.find(L"cddb.cgi")!=base.npos)base=L"https://musicbrainz.org/ws/2/";
    if(!base.starts_with(L"https://") || base.find_first_of(L"?#\r\n")!=base.npos)throw std::runtime_error("MusicBrainz server must be an HTTPS API root");
    if(base.back()!=L'/')base+=L'/';return base;
}
std::string Fetch(const std::wstring& url,const settings::NetworkSettings& network,const DiscCancel& cancel) {
    // One queue per process, shared by automatic/manual and detail requests.
    static std::timed_mutex mutex;static Clock::time_point next;
    struct Cached {std::string body;Clock::time_point expires;};
    static std::map<std::wstring,Cached> cache;
    while(!mutex.try_lock_for(std::chrono::milliseconds(25)))Guard(cancel);
    std::unique_lock lock(mutex,std::adopt_lock);Guard(cancel);
    if(auto it=cache.find(url);it!=cache.end()&&it->second.expires>Clock::now())return it->second.body;
    for(unsigned attempt=0;attempt<3;++attempt) {
        while(Clock::now()<next){Guard(cancel);std::this_thread::sleep_for(std::chrono::milliseconds(25));}
        Guard(cancel);next=Clock::now()+std::chrono::milliseconds(1100);
        const auto agent="TTPlayerRebuild/"+core::WideToUtf8(build::kCompletionDate)+" (https://github.com/Sonic853/TTPlayer)";
        auto response=net::GetHttps(url,agent,network,cancel);Guard(cancel);
        if(response.status==429 || response.status==503) {
            unsigned seconds=2U<<attempt;
            if(!response.retry_after.empty()) {
                char* end{};const auto n=strtoul(response.retry_after.c_str(),&end,10);
                if(end && !*end)seconds=static_cast<unsigned>(std::min<unsigned long>(n,3600));
                else {
                    SYSTEMTIME time{};FILETIME future{},now{};
                    if(WinHttpTimeToSystemTime(core::Utf8ToWide(response.retry_after).c_str(),&time)&&SystemTimeToFileTime(&time,&future)) {
                        GetSystemTimeAsFileTime(&now);
                        ULARGE_INTEGER a{},b{};a.LowPart=future.dwLowDateTime;a.HighPart=future.dwHighDateTime;b.LowPart=now.dwLowDateTime;b.HighPart=now.dwHighDateTime;
                        if(a.QuadPart>b.QuadPart)seconds=static_cast<unsigned>(std::min<ULONGLONG>((a.QuadPart-b.QuadPart)/10000000+1,3600));
                    }
                }
            }
            next=Clock::now()+std::chrono::seconds(std::max(1U,seconds));
            if(seconds>15 || attempt==2)throw std::runtime_error("MusicBrainz is busy; please try again later (HTTP "+std::to_string(response.status)+")");
            continue;
        }
        if(response.status!=200 && response.status!=404)throw std::runtime_error("MusicBrainz HTTP "+std::to_string(response.status));
        if(response.status==404)response.body="{\"releases\":[]}";
        if(cache.size()>=64)cache.erase(cache.begin());
        cache[url]={response.body,Clock::now()+std::chrono::minutes(response.status==404?5:30)};
        return response.body;
    }
    throw std::runtime_error("MusicBrainz request failed");
}
}
DiscQuerySource PrepareDiscQuery(const std::filesystem::path& path,const plugins::PluginManager* manager,HMODULE comm,
    const PlaybackOptions& options,const DiscCancel& cancel) {
    Guard(cancel);DiscQuerySource source;source.path=path;
    if(_wcsicmp(path.extension().c_str(),L".cda")==0)source.layout=ReadDiscLayout(path);
    else if(_wcsicmp(path.extension().c_str(),L".cue")==0) {
        struct MediaLifetime {
            HRESULT result{platform::MFStartup(MF_VERSION, MFSTARTUP_LITE)};
            ~MediaLifetime(){if(SUCCEEDED(result))platform::MFShutdown();}
        } media;
        source.cue=CueSheet::Load(path);const auto& sheet=*source.cue;
        if(sheet.HasDataTracks())throw std::runtime_error("Mixed-mode CUE requires a complete audio-session TOC");
        const auto& tracks=sheet.Tracks();uint64_t file_base=150,inserted=0,last_length=0;
        if(tracks.empty())throw std::runtime_error("CUE has no audio tracks");
        std::wstring current_file;std::vector<std::wstring> completed_files;
        for(size_t i=0;i<tracks.size();++i) {
            Guard(cancel);const auto& track=tracks[i];
            if(!track.has_index01 || track.source_number!=int(i+1))throw std::runtime_error("CUE needs consecutive TRACK numbers and INDEX 01");
            if(_wcsicmp(track.file_reference.c_str(),current_file.c_str())) {
                if(std::find(completed_files.begin(),completed_files.end(),track.file_reference)!=completed_files.end())throw std::runtime_error("CUE reuses a previous FILE");
                file_base+=last_length;completed_files.push_back(track.file_reference);current_file=track.file_reference;
                bool opened=false;
                for(const auto& candidate:sheet.AudioCandidates(track)) {
                    auto audio=CreateDecodedAudioSource(candidate,0,manager,comm);
                    if(audio&&audio->Open(candidate,options)&&audio->Duration().count()>0) {
                        last_length=(uint64_t(audio->Duration().count())*75+500)/1000;opened=true;break;
                    }
                }
                if(!opened)throw std::runtime_error("Cannot determine CUE audio duration");
            }
            if(track.start_frame>=last_length)throw std::runtime_error("CUE index exceeds source duration");
            // First-track PREGAP represents the standard lead-in, already included.
            if(i==0 && track.pregap && track.pregap!=150)throw std::runtime_error("Nonstandard first-track CUE pregap");
            if(i)inserted+=track.pregap;
            const auto start=file_base+track.start_frame+inserted;
            if(start>0x7fffffff || (i && start<=source.layout.tracks.back().start))throw std::runtime_error("Invalid CUE disc offsets");
            if(i)source.layout.tracks.back().end=static_cast<unsigned>(start);
            source.layout.tracks.push_back({static_cast<unsigned>(i+1),static_cast<unsigned>(start),0,false});
            inserted+=track.postgap;
        }
        const auto end=file_base+last_length+inserted;
        if(end>0x7fffffff)throw std::runtime_error("CUE disc is too long");
        source.layout.tracks.back().end=static_cast<unsigned>(end);
    } else throw std::runtime_error("Select an audio CD or CUE track");
    if(!source.layout.IsAudioDisc())throw std::runtime_error("Mixed-mode or invalid TOC cannot be used as an exact audio Disc ID");
    return source;
}
std::vector<DiscCandidate> ParseDiscCandidates(std::string_view json,const DiscLayout& disc) {
    const auto root=update::detail::JsonReader(json).Read();
    if(root["releases"].kind!=Json::array)throw std::runtime_error("MusicBrainz did not return a release list");
    std::vector<DiscCandidate> result;const auto id=disc.MusicBrainzId();
    for(const auto& release:root["releases"].items) {
        const auto mbid=release["id"].String();if(!Mbid(mbid))continue;
        for(const auto& medium:release["media"].items) {
            if(medium["track-count"].Integer()!=disc.tracks.size())continue;
            const auto position=medium["position"].Integer();if(!position || position>99)continue;
            DiscCandidate item;item.release_id=mbid;item.album=Wide(release["title"]);item.artist=Credit(release["artist-credit"]);
            item.date=Wide(release["date"]);item.country=Wide(release["country"]);
            item.medium=static_cast<unsigned>(position);item.total_discs=static_cast<unsigned>(release["media"].items.size());item.tracks=static_cast<unsigned>(disc.tracks.size());
            for(const auto& d:medium["discs"].items)if(d["id"].String()==id)item.exact=true;
            if (root["id"].String()==id && !item.exact) continue;
            for(const auto& label:release["label-info"].items) {
                if(!item.label.empty())item.label+=L" / ";item.label+=Wide(label["label"]["name"])+L" "+Wide(label["catalog-number"]);
            }
            result.push_back(std::move(item));
            if(result.size()>500)throw std::runtime_error("Too many MusicBrainz candidates");
        }
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.exact>b.exact;});
    return result;
}
DiscRelease ParseDiscRelease(std::string_view json,const DiscCandidate& candidate,const DiscLayout& disc) {
    const auto release=update::detail::JsonReader(json).Read();
    if(release["id"].String()!=candidate.release_id)throw std::runtime_error("MusicBrainz release identity changed");
    DiscRelease result;result.candidate=candidate;
    for(const auto& medium:release["media"].items)if(medium["position"].Integer()==candidate.medium) {
        if(medium["tracks"].items.size()!=disc.tracks.size())throw std::runtime_error("MusicBrainz track count does not match the source");
        for(const auto& track:medium["tracks"].items) {
            const auto position=track["position"].Integer();
            if(!position || position>disc.tracks.size())throw std::runtime_error("Invalid MusicBrainz track position");
            const auto number=disc.tracks[static_cast<size_t>(position-1)].number;
            if(result.tracks.contains(static_cast<unsigned>(number)))throw std::runtime_error("Duplicate MusicBrainz track position");
            auto& fields=result.tracks[number];const auto& recording=track["recording"];
            auto title=Wide(track["title"]);if(title.empty())title=Wide(recording["title"]);
            auto artist=Credit(track["artist-credit"]);if(artist.empty())artist=Credit(recording["artist-credit"]);if(artist.empty())artist=candidate.artist;
            Add(fields,L"Title",title);Add(fields,L"Artist",artist);Add(fields,L"Album",candidate.album);Add(fields,L"AlbumArtist",candidate.artist);
            Add(fields,L"Date",candidate.date);Add(fields,L"Discnumber",std::to_wstring(candidate.medium));
            Add(fields,L"Disctotal",std::to_wstring(release["media"].items.size()));
            Add(fields,L"MusicBrainz_AlbumId",core::Utf8ToWide(candidate.release_id));
            Add(fields,L"MusicBrainz_TrackId",Wide(recording["id"]));Add(fields,L"MusicBrainz_DiscId",core::Utf8ToWide(disc.MusicBrainzId()));
            std::wstring genres;for(const auto& genre:release["genres"].items){if(!genres.empty())genres+=L"; ";genres+=Wide(genre["name"]);}Add(fields,L"Genre",genres);
        }
        return result;
    }
    throw std::runtime_error("Selected MusicBrainz medium is missing");
}
std::vector<DiscCandidate> QueryMusicBrainz(const DiscLayout& disc,const settings::NetworkSettings& network,const DiscCancel& cancel) {
    const auto url=Base(network)+L"discid/"+core::Utf8ToWide(disc.MusicBrainzId())+
        L"?fmt=json&inc=artist-credits+labels&cdstubs=no&toc="+disc.TocParameter();
    return ParseDiscCandidates(Fetch(url,network,cancel),disc);
}
DiscRelease ReadMusicBrainzRelease(const DiscCandidate& candidate,const DiscLayout& disc,const settings::NetworkSettings& network,const DiscCancel& cancel) {
    if(!Mbid(candidate.release_id))throw std::runtime_error("Invalid release MBID");
    return ParseDiscRelease(Fetch(Base(network)+L"release/"+core::Utf8ToWide(candidate.release_id)+
        L"?fmt=json&inc=recordings+artist-credits+discids+genres",network,cancel),candidate,disc);
}
void SaveDiscRelease(const DiscQuerySource& source,const DiscRelease& release) {
    if(release.tracks.size()!=source.layout.tracks.size())throw std::runtime_error("Incomplete disc metadata");
    if(source.cue) {
        std::vector<std::pair<int,CueMetadata>> changes;
        bool first=true;
        for(const auto& [number,fields]:release.tracks) {
            CueMetadata edits;
            for(const auto& field:fields) {
                if(!first && (!_wcsicmp(field.first.c_str(),L"Album") || !_wcsicmp(field.first.c_str(),L"AlbumArtist")))continue;
                edits.push_back(field);
            }
            changes.emplace_back(static_cast<int>(number),std::move(edits));first=false;
        }
        source.cue->WriteDiscMetadata(changes);
    } else {
        if(ReadDiscLayout(source.path)!=source.layout)throw std::runtime_error("CD was replaced before saving");
        WriteDiscMetadata(source.layout,release.tracks);
    }
}
}
