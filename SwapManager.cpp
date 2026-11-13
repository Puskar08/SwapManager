#include "pch.h"
#include "SwapManager.h"

//+------------------------------------------------------------------+
//| Constructor                                                      |
//+------------------------------------------------------------------+
SwapManager::SwapManager(void) : m_api(NULL), m_config(NULL), m_config_param(NULL)
{
	ZeroMemory(&m_info, sizeof(m_info));
}

//+------------------------------------------------------------------+
//| Destructor                                                       |
//+------------------------------------------------------------------+
SwapManager::~SwapManager(void)
{
	Stop();
}

//+------------------------------------------------------------------+
//| Release object footprint memory allocation                       |
//+------------------------------------------------------------------+
void SwapManager::Release(void)
{
	// Cleaning up the rules tracking memory layout storage vector before deleting the class instance
	{
		std::lock_guard<std::mutex> lock(m_rules_lock);
		m_rules.clear();
		m_rules.shrink_to_fit(); // Forces the operating system to reclaim rule cache RAM spaces
	}
	delete this;
}

//+------------------------------------------------------------------+
//| Plugin Entry Point                                               |
//+------------------------------------------------------------------+
MTAPIRES SwapManager::Start(IMTServerAPI* server)
{
	if (!server) return MT_RET_ERR_PARAMS;
	m_api = server;

	// Register allocation metrics
	if ((m_config = m_api->PluginCreate()) == nullptr) {
		return MT_RET_ERR_MEM;
	}

	MTAPIRES retcode = ParametersRead();
	if (retcode != MT_RET_OK)
	{
		Stop();
		return(retcode);
	}

	wcscpy_s(m_plugin_name, m_config->Name());

	//--- subscribe
	if (m_api->PluginSubscribe(this) != MT_RET_OK
		|| m_api->TradeSubscribe(this) != MT_RET_OK)
	{
		Stop();
		return(MT_RET_ERROR);
	}

	return MT_RET_OK;
}

//+------------------------------------------------------------------+
//| Plugin Shutdown Termination                                      |
//+------------------------------------------------------------------+
MTAPIRES SwapManager::Stop(void)
{
	if (m_api)
	{
		m_api->TradeUnsubscribe(this);
		m_api->PluginUnsubscribe(this);
	}

	if (m_config_param) { m_config_param->Release(); m_config_param = nullptr; }
	if (m_config) { m_config->Release();       m_config = nullptr; }

	m_api = NULL;

	return MT_RET_OK;
}

//+------------------------------------------------------------------+
//| Real-time Dashboard Runtime Configurations Notification Update    |
//+------------------------------------------------------------------+
void SwapManager::OnPluginUpdate(const IMTConPlugin* plugin)
{
	if (!plugin || !m_config) return;

	if (CMTStr::Compare(plugin->Name(), m_config->Name()) == 0 && plugin->Server() == m_config->Server())
	{
		MTAPIRES retcode = ParametersRead();
		if (retcode != MT_RET_OK)
		{
			Stop();
		}
	}
}

//+------------------------------------------------------------------+
//| Loop configuration settings metrics across parameter arrays      |
//+------------------------------------------------------------------+
MTAPIRES SwapManager::ParametersRead()
{
	if (!m_api) return (MT_RET_ERR_PARAMS);

	if (!m_config) {
		if ((m_config = m_api->PluginCreate()) == NULL) {
			return (MT_RET_ERR_MEM);
		}
	}

	if (!m_config_param) {
		if ((m_config_param = m_api->PluginParamCreate()) == NULL) {
			return (MT_RET_ERR_MEM);
		}
	}

	MTAPIRES retcode = m_api->PluginCurrent(m_config);
	if (retcode != MT_RET_OK) return (retcode);

	wchar_t current_name[64] = { 0 };
	if (m_config->Name() && m_config->Name()[0] != L'\0') {
		wcscpy_s(current_name, m_config->Name());
	}
	else {
		wcscpy_s(current_name, L"SwapManagerPlugin"); // Safe static fallback string if empty
	}

	std::vector<SwapManagerRule> temporary_rules;
	const UINT total = m_config->ParameterTotal();

	for (UINT i = 0; i < total; i++) {
		if (m_config->ParameterNext(i, m_config_param) != MT_RET_OK)
			continue;

		LPCWSTR param_name = m_config_param->Name();
		if (!param_name) continue;

		if (m_config_param->Type() != IMTConParam::TYPE_STRING)
			continue;

		SwapManagerRule rule;
		if (CompileRuleString(param_name, m_config_param->ValueString(), rule)) {
			temporary_rules.push_back(rule);
		}
		else {
			m_api->LoggerOut(MTLogErr, L"[%s] PARAMETER ERROR: Invalid swap rule [%s] provided in configurations. Plugin aborted", current_name, m_config_param->ValueString());
			return (MT_RET_ERR_PARAMS);
		}
	}

	// Lock and contextually shift temporary vector allocation into global collection fields
	{
		std::lock_guard<std::mutex> lock(m_rules_lock);
		m_rules = std::move(temporary_rules);
	}

	return MT_RET_OK;
}


//+------------------------------------------------------------------+
//| String Token Parameter Extraction Helper                        |
//+------------------------------------------------------------------+
bool SwapManager::GetParamValue(const std::wstring& line, const std::wstring& param, std::wstring& out_val)
{
	if (line.empty() || param.empty()) return false;

	LPCWSTR haystack = line.c_str();
	size_t param_len = param.length();

	// 1. Lightning fast case-insensitive key search loop
	while (*haystack) {
		// Look for matching parameter prefix key
		if (_wcsnicmp(haystack, param.c_str(), param_len) == 0) {
			// Check if the next non-space character is the colon separator
			LPCWSTR colon_check = haystack + param_len;
			while (*colon_check == L' ') colon_check++;

			if (*colon_check == L':') {
				// Key found! Move pointer right past the colon to the raw value string
				LPCWSTR value_ptr = colon_check + 1;

				// Trim leading whitespace instantly on the pointer level (zero copying)
				while (*value_ptr == L' ') value_ptr++;

				// Track where the value string content terminates
				LPCWSTR end_ptr = value_ptr;
				while (*end_ptr && *end_ptr != L';') {
					end_ptr++;
				}

				// Backtrack the end pointer to strip trailing whitespace instantly
				while (end_ptr > value_ptr && (*(end_ptr - 1) == L' ' || *(end_ptr - 1) == L'\r' || *(end_ptr - 1) == L'\n')) {
					end_ptr--;
				}

				size_t final_len = end_ptr - value_ptr;
				if (final_len == 0) return false;

				// The ONLY memory allocation performed: assign the structural range to out_val
				out_val.assign(value_ptr, final_len);
				return true;
			}
		}

		// Move pointer forward past the current token parameter parameter sequence boundary
		while (*haystack && *haystack != L';') {
			haystack++;
		}
		if (*haystack == L';') {
			haystack++; // Skip the semicolon delimiter marker
		}
	}

	return false;
}

//+------------------------------------------------------------------+
//| Complex Math Syntax Evaluation Engine Parsing                    |
//+------------------------------------------------------------------+
bool SwapManager::ParseSwapValue(const std::wstring& str, SwapValue& out_val)
{
	if (str.empty()) return false;

	// Low-latency stack clear instead of slow ZeroMemory call
	out_val.math_type = MATH_PER_TRADE;
	out_val.value = 0.0;
	out_val.currency[0] = L'\0';

	LPCWSTR start_ptr = str.c_str();
	wchar_t* end_ptr = nullptr;

	// 1. Instantly parse the double numeric value directly from memory (zero copying)
	double parsed_value = wcstod(start_ptr, &end_ptr);
	if (end_ptr == start_ptr) return false; // Exit immediately if no numeric value found

	out_val.value = parsed_value;

	// 2. Skip over any accidental padding whitespace following the number
	while (*end_ptr == L' ') end_ptr++;

	// 3. Match Pattern Mode A: Percentage Check
	if (*end_ptr == L'%') {
		out_val.math_type = MATH_PERCENTAGE;
		return true;
	}

	// 4. Match Pattern Modes B & C: Extract raw currency codes directly via hardware loops
	size_t curr_idx = 0;
	// Copy alphabetic characters directly into the buffer, leaving space for null-terminator
	while (curr_idx < 15 && ((*end_ptr >= L'A' && *end_ptr <= L'Z') || (*end_ptr >= L'a' && *end_ptr <= L'z'))) {
		out_val.currency[curr_idx++] = *end_ptr++;
	}
	out_val.currency[curr_idx] = L'\0';

	// 5. Skip over whitespace following the currency code
	while (*end_ptr == L' ') end_ptr++;

	// 6. Deduce whether it is Per-Lot or Per-Trade using zero-allocation case-insensitive match
	if (_wcsnicmp(end_ptr, L"/lot", 4) == 0) {
		out_val.math_type = MATH_PER_LOT;
	}
	else {
		out_val.math_type = MATH_PER_TRADE;
	}

	return true;
}

//+------------------------------------------------------------------+
//| Configuration String Compiler Mapping Processor                   |
//+------------------------------------------------------------------+
bool SwapManager::CompileRuleString(LPCWSTR param_name, LPCWSTR param_value, SwapManagerRule& rule)
{
	if (!param_name || !param_value || param_value[0] == L'\0') return false;

	// Reset the output object instance
	rule = SwapManagerRule();
	rule.rule_name = param_name;

	std::wstring content(param_value);
	std::wstring val_str;

	// Extract filter scopes
	if (!GetParamValue(content, L"accounts", val_str)) return false;
	CompileMaskString(val_str.c_str(), rule.account_tokens);

	if (!GetParamValue(content, L"symbols", val_str)) return false;
	CompileMaskString(val_str.c_str(), rule.symbol_tokens);

	// Extract pricing values
	if (!GetParamValue(content, L"swaps", val_str) || !ParseSwapValue(val_str, rule.swap_config)) return false;

	// Extract validation modes
	rule.mode = MODE_NONE;
	if (GetParamValue(content, L"mode", val_str)) {
		if (_wcsicmp(val_str.c_str(), L"registration") == 0) rule.mode = MODE_REGISTRATION;
		else if (_wcsicmp(val_str.c_str(), L"trade") == 0) rule.mode = MODE_TRADE;
	}

	// Extract grace configuration counters
	rule.free_units = 0;
	if (GetParamValue(content, L"free", val_str)) {
		rule.free_units = _wtoi(val_str.c_str());
	}

	// Extract calendar transformation structures
	rule.rollover = ROLLOVER_DAILY;
	if (GetParamValue(content, L"rollover", val_str)) {
		if (_wcsicmp(val_str.c_str(), L"bank") == 0) rule.rollover = ROLLOVER_BANK;
	}

	// Extract operational balance targets execution methods
	rule.charge_type = CHARGE_TYPE_SWAP;
	if (GetParamValue(content, L"type", val_str)) {
		if (_wcsicmp(val_str.c_str(), L"trans") == 0) rule.charge_type = CHARGE_TYPE_TRANS;
	}

	return true;
}

//+------------------------------------------------------------------+
//| Case-insensitive Substring String Extraction Subroutine Matcher  |
//+------------------------------------------------------------------+
LPCWSTR SwapManager::CustomWcscasestr(LPCWSTR haystack, LPCWSTR needle)
{
	if (!*needle) return haystack;
	for (; *haystack; ++haystack) {
		if (towlower(*haystack) == towlower(*needle)) {
			LPCWSTR h = haystack, n = needle;
			while (*h && *n && towlower(*h) == towlower(*n)) {
				h++;
				n++;
			}
			if (!*n) return haystack;
		}
	}
	return nullptr;
}

//+------------------------------------------------------------------+
//| Element Token Logic Evaluation Matching Router Engine           |
//+------------------------------------------------------------------+
bool SwapManager::EvaluateCondition(const TokenCondition& cond, LPCWSTR value, size_t value_len)
{
	if (cond.type == MATCH_ALL) return true;
	if (value_len < cond.pattern_len) return false;

	switch (cond.type) {
	case MATCH_EXACT:
		if (value_len != cond.pattern_len) return false;
		return (_wcsicmp(value, cond.pattern) == 0);

	case MATCH_PREFIX:
		return (_wcsnicmp(value, cond.pattern, cond.pattern_len) == 0);

	case MATCH_SUFFIX:
		return (_wcsicmp(value + (value_len - cond.pattern_len), cond.pattern) == 0);

	case MATCH_CONTAINS:
		return (CustomWcscasestr(value, cond.pattern) != nullptr);
	}
	return false;
}

//+------------------------------------------------------------------+
//| Token Multi-Condition Logic Match Aggregation Subroutine Loop    |
//+------------------------------------------------------------------+
bool SwapManager::EvaluateTokenList(const std::vector<TokenCondition>& tokens, LPCWSTR value)
{
	if (tokens.empty()) return false;

	size_t value_len = wcslen(value);
	bool match = false;
	bool has_positive_rules = false;

	for (const auto& cond : tokens) {
		if (cond.is_negated) {
			if (EvaluateCondition(cond, value, value_len)) {
				return false; // Absolute elimination prioritizes path rules match termination
			}
		}
		else {
			has_positive_rules = true;
			if (!match && EvaluateCondition(cond, value, value_len)) {
				match = true;
			}
		}
	}
	return !has_positive_rules ? true : match;
}

//+------------------------------------------------------------------+
//| Pattern Character Vector Compilation Subroutine Tokenizer        |
//+------------------------------------------------------------------+
void SwapManager::CompileMaskString(LPCWSTR mask_str, std::vector<TokenCondition>& target_list)
{
	if (!mask_str || mask_str[0] == L'\0') return;

	wchar_t buffer[512];
	wcscpy_s(buffer, mask_str);

	wchar_t* next_token = nullptr;
	wchar_t* token = wcstok_s(buffer, L",", &next_token);

	while (token != nullptr) {
		while (*token == L' ') token++;
		size_t len = wcslen(token);
		while (len > 0 && token[len - 1] == L' ') {
			token[len - 1] = L'\0';
			len--;
		}

		if (len == 0) {
			token = wcstok_s(nullptr, L",", &next_token);
			continue;
		}

		TokenCondition cond = {};
		cond.is_negated = false;

		if (*token == L'!') {
			cond.is_negated = true;
			token++;
			len--;
		}

		if (wcscmp(token, L"*") == 0) {
			cond.type = MATCH_ALL;
			cond.pattern[0] = L'\0';
		}
		else if (token[0] == L'*' && token[len - 1] == L'*' && len > 2) {
			cond.type = MATCH_CONTAINS;
			token[len - 1] = L'\0';
			wcscpy_s(cond.pattern, token + 1);
		}
		else if (token[0] == L'*') {
			cond.type = MATCH_SUFFIX;
			wcscpy_s(cond.pattern, token + 1);
		}
		else if (token[len - 1] == L'*') {
			cond.type = MATCH_PREFIX;
			token[len - 1] = L'\0';
			wcscpy_s(cond.pattern, token);
		}
		else {
			cond.type = MATCH_EXACT;
			wcscpy_s(cond.pattern, token);
		}

		cond.pattern_len = wcslen(cond.pattern);
		target_list.push_back(cond);

		token = wcstok_s(nullptr, L",", &next_token);
	}
}


//+------------------------------------------------------------------+
//| Dynamic Core Swap Overrides Implementation Hook                  |
//+------------------------------------------------------------------+
MTAPIRES SwapManager::HookTradeRollover(const int64_t datetime,
	const IMTConGroup* group,
	const IMTConSymbol* symbol,
	const IMTPosition* position,
	const double original_value,
	double& new_value)
{
	if (!group || !symbol || !position || !m_api) return MT_RET_OK;

	UINT64 login = position->Login();
	wchar_t login_str[64];
	swprintf_s(login_str, L"%I64u", login);
	LPCWSTR group_name = group->Group();
	LPCWSTR symbol_name = symbol->Symbol();

	SwapManagerRule active_rule = {};
	bool rule_found = false;

	// Thread-safe extraction lookup matching parameters loop execution
	{
		std::lock_guard<std::mutex> lock(m_rules_lock);
		for (const auto& rule : m_rules) {
			bool match_account = EvaluateTokenList(rule.account_tokens, group_name) ||
				EvaluateTokenList(rule.account_tokens, login_str);
			bool match_symbol = EvaluateTokenList(rule.symbol_tokens, symbol_name);

			if (match_account && match_symbol) {
				active_rule = rule;
				rule_found = true;
				break;
			}
		}
	}

	if (!rule_found) return MT_RET_OK; // Fallback to native system parameters

	// Grace Period Verification Processing
	bool inside_free_period = false;
	if (active_rule.mode != MODE_NONE && active_rule.free_units > 0) {

		int64_t reference_time = 0;
		if (active_rule.mode == MODE_REGISTRATION) {
			IMTUser* user = m_api->UserCreate();
			if (user) {
				if (m_api->UserGet(login, user) == MT_RET_OK) {
					reference_time = user->Registration();
				}
				user->Release();
			}
		}
		else if (active_rule.mode == MODE_TRADE) {
			reference_time = position->TimeCreate();
		}

		if (reference_time > 0) {
			if (active_rule.rollover == ROLLOVER_DAILY) {
				int64_t elapsed_seconds = datetime - reference_time;
				int elapsed_days = static_cast<int>(elapsed_seconds / (24 * 3600));
				if (elapsed_days < active_rule.free_units) {
					inside_free_period = true;
				}
			}
			else if (active_rule.rollover == ROLLOVER_BANK) {
				// 1. Read how many units this specific position has already used in past rollovers
				int past_units_spent = 0;
				wchar_t comment_buffer[32] = { 0 };
				wcscpy_s(comment_buffer, position->Comment());

				// Check if we previously stamped our custom tracker token (e.g., "[SWU:4]") into the position comment
				LPCWSTR tracker_ptr = wcsstr(comment_buffer, L"[SWU:");
				if (tracker_ptr != nullptr) {
					past_units_spent = _wtoi(tracker_ptr + 5);
				}

				// 2. Get ONLY today's exact multiplier right now
				int todays_multiplier = GetSymbolSwapMultiplier(symbol, datetime);

				// 3. Determine if the position is inside its swap-free balance pool
				if (past_units_spent < active_rule.free_units) {
					inside_free_period = true;

					// Calculate how many units we are consuming during this specific rollover event
					int units_consumed_today = todays_multiplier;

					// If today's multiplier (e.g., x3 Wednesday) is larger than our remaining free pool, 
					// we cap our tracking deduction to match what is left of the free limit.
					if (past_units_spent + units_consumed_today > active_rule.free_units) {
						units_consumed_today = active_rule.free_units - past_units_spent;
					}

					// Update the accumulated tracker metric for the next day's execution cycle
					int new_total_spent = past_units_spent + units_consumed_today;

					// We write this updated state directly back to the MT5 platform engine storage
					IMTPosition* mutable_pos = const_cast<IMTPosition*>(position);
					wchar_t new_comment[128];

					// Clean out old tracking token if it exists to prevent comment string bloat
					if (tracker_ptr != nullptr) {
						// Cut the comment string right before our tracking token
						size_t base_len = tracker_ptr - comment_buffer;
						comment_buffer[base_len] = L'\0';
					}

					// Append the updated tracking tag securely
					swprintf_s(new_comment, L"%s[SWU:%d]", comment_buffer, new_total_spent);
					mutable_pos->Comment(new_comment);

					// Push the modification directly up into the active MT5 trade engine core cache
					m_api->PositionUpdate(mutable_pos);
				}
			}
		}
	}

	// Determine final math charge targets parameters
	double computed_swap = original_value;
	if (inside_free_period) {
		computed_swap = 0.0;
	}
	else {
		double volume_lots = position->Volume() / 10000.0; // MT5 normalized volume scaling
		switch (active_rule.swap_config.math_type) {
		case MATH_PERCENTAGE:
			computed_swap = original_value + (original_value * (active_rule.swap_config.value / 100.0));
			break;
		case MATH_PER_LOT:
			computed_swap = active_rule.swap_config.value * volume_lots;
			break;
		case MATH_PER_TRADE:
			computed_swap = active_rule.swap_config.value;
			break;
		}
	}

	// Execution Routing Channels Configuration Output
	if (active_rule.charge_type == CHARGE_TYPE_TRANS) {
		new_value = 0.0; // Clear visible UI swap display fields
		if (computed_swap != 0.0) {
			wchar_t comment[128];
			swprintf_s(comment, L"Swap Manager Adjust - Pos #%I64u", position->Position());
			ExecuteBalanceTransaction(login, computed_swap, comment, symbol);
		}
	}
	else {
		new_value = computed_swap; // Post results directly to positions array
	}

	return MT_RET_OK;
}

//+------------------------------------------------------------------+
//| Dynamic Extraction Processing for MT5 Symbol Day Multipliers      |
//+------------------------------------------------------------------+
int SwapManager::GetSymbolSwapMultiplier(const IMTConSymbol* symbol, const int64_t datetime)
{
	if (!symbol) return 1;

	SYSTEMTIME st;
	FILETIME ft;
	INT64 ll = (datetime + 11644473600LL) * 10000000LL;
	ft.dwLowDateTime = (DWORD)ll;
	ft.dwHighDateTime = (DWORD)(ll >> 32);
	FileTimeToSystemTime(&ft, &st);

	// Map DayOfWeek tracking indices securely against MT5 enumeration standards
	switch (st.wDayOfWeek) {
	case 1: return symbol->SwapRateMonday();
	case 2: return symbol->SwapRateTuesday();
	case 3: return symbol->SwapRateWednesday();
	case 4: return symbol->SwapRateThursday();
	case 5: return symbol->SwapRateFriday();
	case 6: return symbol->SwapRateSaturday();
	case 0: return symbol->SwapRateSunday();
	}
	return 1;
}

//+------------------------------------------------------------------+
//| Post Balance Adjustments Transactions Routing Module             |
//+------------------------------------------------------------------+
bool SwapManager::ExecuteBalanceTransaction(UINT64 login, double amount, LPCWSTR comment, const IMTConSymbol* symbol)
{
	if (!m_api || !symbol) return false;

	IMTDeal* request = m_api->DealCreate();
	if (!request) return false;

	request->Clear();
	request->Login(login);
	request->Dealer(0);
	request->Order(0);
	request->Action(IMTDeal::DEAL_BALANCE);
	request->Entry(0);
	request->ReasonSet(IMTDeal::DEAL_REASON_DEALER);
	request->Digits(2);
	request->DigitsCurrency(2);
	request->ContractSize(0);
	request->Price(0);
	request->Volume(0);
	request->Profit(amount);
	request->Comment(comment);

	/*wchar_t dynamic_comment[128] = { 0 };
	swprintf_s(dynamic_comment, L"Swap Manger - Swap for [%s]", symbol->Symbol());*/

	MTAPIRES request_result = m_api->DealAdd(request);
	if (request_result != MT_RET_OK) {
		m_api->LoggerOut(MTLogErr, L"Swap applied by Transaction for [%s]->login [%I64u]",
			symbol->Symbol(), login);
	}
	else {
		m_api->LoggerOut(MTLogErr, L"Swap apply by Transaction failed for [%s]->login [%I64u]",
			symbol->Symbol(), login);
	}
	request->Release();

	return (request_result == MT_RET_OK);
}
