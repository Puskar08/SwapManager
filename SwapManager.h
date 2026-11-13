#pragma once
#include <windows.h>
#include <vector>
#include <mutex>
#include <cwctype>
#include <string>
//#include <algorithm>


// Matching criteria types for standard MT5 configurations
enum MatchType {
	MATCH_ALL,
	MATCH_PREFIX,
	MATCH_SUFFIX,
	MATCH_CONTAINS,
	MATCH_EXACT
};

// Structural container for evaluated string expressions
struct TokenCondition {
	bool is_negated;
	MatchType type;
	wchar_t pattern[64];
	size_t pattern_len;
};

// Mode parameter variations
enum SwapMode {
	MODE_NONE,
	MODE_REGISTRATION,
	MODE_TRADE
};

// Rollover tracking variations
enum RolloverType {
	ROLLOVER_DAILY,
	ROLLOVER_BANK
};

// Charging transaction distribution options
enum ChargeType {
	CHARGE_TYPE_SWAP,   // Displayed in "Swaps" column, impacts floating PnL
	CHARGE_TYPE_TRANS   // Nullify MT5 visual swap, inject a deposit/withdrawal deal
};

// Swap math modifier logic
enum SwapMathType {
	MATH_PER_TRADE,     // Fixed fee per transaction
	MATH_PER_LOT,       // Fixed fee multiplied by volume
	MATH_PERCENTAGE     // Markup/markdown applied to base group symbol swap
};

// Container for parsed multi-format currency/value attributes
struct SwapValue {
	SwapMathType math_type;
	double value;
	wchar_t currency[16];
};

// Unified configuration rule template block
struct SwapManagerRule {
	std::wstring rule_name;

	// Filtering Layer (Reuses your TradeCopy TokenCondition logic)
	std::vector<TokenCondition> account_tokens;
	std::vector<TokenCondition> symbol_tokens;

	// Parameter Configuration Layer
	SwapValue swap_config;
	int free_units;
	SwapMode mode;
	RolloverType rollover;
	ChargeType charge_type;
};


class SwapManager : public IMTServerPlugin, public IMTConPluginSink, public IMTTradeSink
{
private:
	// Server API pointer
	MTServerInfo m_info;
	IMTServerAPI* m_api;

	// Plugin metadata configurations
	IMTConPlugin* m_config;
	IMTConParam* m_config_param;
	wchar_t                     m_plugin_name[64];

	// Thread-safe runtime parameters architecture
	std::vector<SwapManagerRule> m_rules;
	std::mutex                  m_rules_lock;

	// Static low-latency character evaluation matching utilities
	static LPCWSTR CustomWcscasestr(LPCWSTR haystack, LPCWSTR needle);
	static bool EvaluateCondition(const TokenCondition& cond, LPCWSTR value, size_t value_len);
	static bool EvaluateTokenList(const std::vector<TokenCondition>& tokens, LPCWSTR value);
	static void CompileMaskString(LPCWSTR mask_str, std::vector<TokenCondition>& target_list);

	static void NormalizeParamName(LPCWSTR src, wchar_t* dest, size_t dest_max_len);

	// String manipulation parsing internals
	static bool GetParamValue(const std::wstring& line, const std::wstring& param, std::wstring& out_val);
	static bool ParseSwapValue(const std::wstring& str, SwapValue& out_val);
	bool CompileRuleString(LPCWSTR param_name, LPCWSTR param_value, SwapManagerRule& rule);

	// Core platform calculation dependencies
	int GetSymbolSwapMultiplier(const IMTConSymbol* symbol, const int64_t datetime);
	bool ExecuteBalanceTransaction(UINT64 login, double amount, LPCWSTR comment, const IMTConSymbol* symbol);

public:
	SwapManager(void);
	virtual ~SwapManager(void);

	//--- IMTServerPlugin interface lifecycle implementations
	virtual void      Release(void) override;
	virtual MTAPIRES  Start(IMTServerAPI* server) override;
	virtual MTAPIRES  Stop(void) override;

	//--- IMTConPluginSink interface callback implementation
	virtual void      OnPluginUpdate(const IMTConPlugin* plugin) override;

	//--- Core MT5 Dynamic Swap Multiplier Override Integration Hook
	virtual MTAPIRES  HookTradeRollover(const int64_t datetime,
		const IMTConGroup* group,
		const IMTConSymbol* symbol,
		const IMTPosition* position,
		const double        original_value,
		double& new_value);

	// Configuration string extraction operations
	MTAPIRES          ParametersRead();
};
