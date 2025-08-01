#pragma once
#include "AbstractRoguelikeTaskPlugin.h"

namespace asst
{

class RoguelikeIterateMonthlySquadPlugin : public AbstractRoguelikeTaskPlugin
{
public:
    using AbstractRoguelikeTaskPlugin::AbstractRoguelikeTaskPlugin;
    virtual ~RoguelikeIterateMonthlySquadPlugin() override = default;
    virtual bool verify(AsstMsg msg, const json::value& details) const override;
    virtual bool load_params(const json::value& params) override;

protected:
    virtual bool _run() override;

private:
    // 月度小队数量
    std::unordered_map<std::string, int> monthlySquadCount = { { "Phantom", 8 },
                                                               { "Mizuki", 8 },
                                                               { "Sami", 8 },
                                                               { "Sarkaz", 8 },
                                                               { "JieGarden", 8 } };
    bool m_checkComms;
    bool m_completed;
    bool m_iterateMS;
    virtual bool try_task(const char*) const;
};

}
