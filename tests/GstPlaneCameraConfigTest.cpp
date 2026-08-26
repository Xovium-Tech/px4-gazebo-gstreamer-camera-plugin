/****************************************************************************
 *
 *   Copyright (c) 2026 omniLink. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be used to
 *    endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "GstPlaneCameraConfig.hpp"

#include <iostream>
#include <string>

namespace
{

bool ExpectEqual(const std::string &_actual, const std::string &_expected, const char *_case)
{
	if (_actual == _expected) {
		return true;
	}

	std::cerr << _case << ": expected [" << _expected << "], got [" << _actual << "]\n";
	return false;
}

} // namespace

int main()
{
	bool passed = true;
	passed &= ExpectEqual(custom::detail::DefaultUdpHost(nullptr), "127.0.0.1", "unset host");
	passed &= ExpectEqual(custom::detail::DefaultUdpHost(""), "127.0.0.1", "empty host");
	passed &= ExpectEqual(
		custom::detail::DefaultUdpHost("192.168.1.100"), "192.168.1.100", "configured host");
	return passed ? 0 : 1;
}
