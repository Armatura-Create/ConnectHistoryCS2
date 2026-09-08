#include "mm/chat.h"

#include "mm/globals.h"

#include <engine/igameeventsystem.h>
#include <irecipientfilter.h>
#include <networksystem/inetworkmessages.h>
#include <networksystem/inetworkserializer.h>
// CNetMessagePB и ToPB<>: inetworkserializer.h объявляет CNetMessage только
// вперёд, а нам нужно приведение и delete.
#include <networksystem/netmessage.h>

#include "usermessages.pb.h"

namespace ch {
namespace chatmsg {
namespace {

// Получатель ровно один.
//
// Свой фильтр, а не перегрузка PostEventAbstract с маской uint64: комментарий
// к ней в igameeventsystem.h говорит, что бит соответствует «client index - 1»,
// а плагины ставят бит по самому слоту. Расхождение в документации движка — не
// то, на чём стоит угадывать, когда рядом есть однозначный интерфейс.
class SingleRecipientFilter final : public IRecipientFilter {
public:
    explicit SingleRecipientFilter(int slot) {
        _recipients.ClearAll();
        _recipients.Set(slot);
    }

    NetChannelBufType_t GetNetworkBufType() const override { return BUF_RELIABLE; }
    bool IsInitMessage() const override { return false; }
    const CPlayerBitVec& GetRecipients() const override { return _recipients; }
    CPlayerSlot GetPredictedPlayerSlot() const override { return -1; }

private:
    CPlayerBitVec _recipients;
};

// Тип сообщения живёт столько же, сколько процесс, — ищем один раз.
INetworkMessageInternal* SayText2Type() {
    static INetworkMessageInternal* type = nullptr;

    if (type == nullptr && g_networkMessages != nullptr) {
        type = g_networkMessages->FindNetworkMessage("CUserMessageSayText2");
    }
    return type;
}

}  // namespace

bool SendToSlot(int slot, const std::string& text) {
    if (slot < 0 || slot >= ABSOLUTE_PLAYER_LIMIT) return false;
    if (g_gameEventSystem == nullptr) return false;

    INetworkMessageInternal* type = SayText2Type();
    if (type == nullptr) return false;

    // Сообщение создаёт движок, а не мы: CNetMessagePB требует привязку
    // к протобуфу, идентификатор и группу, которые руками не собрать. Порядок
    // описан прямо в public/networksystem/netmessage.h.
    CNetMessage* allocated = type->AllocateMessage();
    if (allocated == nullptr) return false;

    CNetMessagePB<CUserMessageSayText2>* message =
        allocated->ToPB<CUserMessageSayText2>();

    // entityindex = -1 означает «сообщение не от игрока», chat = false — что
    // строку не нужно прогонять через фильтр чата и звук.
    message->set_entityindex(-1);
    message->set_chat(false);
    message->set_messagename(text);

    SingleRecipientFilter filter(slot);
    g_gameEventSystem->PostEventAbstract(0, false, &filter, type, message, 0);

    delete allocated;
    return true;
}

}  // namespace chatmsg
}  // namespace ch
