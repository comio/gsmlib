// *************************************************************************
// * GSM TA/ME library
// *
// * File:    gsm_me_ta.cc
// *
// * Purpose: Mobile Equipment/Terminal Adapter functions
// *          (ETSI GSM 07.07)
// *
// * Author:  Peter Hofmann (software@pxh.de)
// *
// * Created: 10.5.1999
// *************************************************************************

#ifdef HAVE_CONFIG_H
#include <gsm_config.h>
#endif
#include <gsmlib/gsm_nls.h>
#include <gsmlib/gsm_me_ta.h>
#include <gsmlib/gsm_parser.h>
#include <gsmlib/gsm_sysdep.h>
#include <algorithm>
#include <cstdlib>

using namespace gsmlib;

// Capabilities members

Capabilities::Capabilities() :
  _hasSMSSCAprefix(true),
  _cpmsParamCount(-1),          // initialize to -1, must be set later by
                                // setSMSStore() function
  _omitsColon(true),            // FIXME
  _veryShortCOPSanswer(false),  // Falcom A2-1
  _wrongSMSStatusCode(false),   // Motorola Timeport 260
  _CDSmeansCDSI(false),         // Nokia Cellular Card Phone RPE-1 GSM900 and
                                // Nokia Card Phone RPM-1 GSM900/1800
  _sendAck(false)               // send ack for directly routed SMS
{
}

// MeTa members

void MeTa::init() throw(GsmException)
{
  // switch on extended error codes
  // caution: may be ignored by some TAs, so allow it to fail
  _at->chat("+CMEE=1", "", true, true);
  
  // select SMS pdu mode
  _at->chat("+CMGF=0");

  // now fill in capability object
  MEInfo info = getMEInfo();
  
  // Ericsson model 6050102
  if ((info._manufacturer == "ERICSSON" &&
      (info._model == "1100801" ||
       info._model == "1140801")) ||
      getenv("GSMLIB_SH888_FIX") != NULL)
  {
    // the Ericsson leaves out the service centre address
    _capabilities._hasSMSSCAprefix = false;
  }

  // handle Falcom strangeness
  if ((info._manufacturer == "Funkanlagen Leipoldt OHG" &&
      info._revision == "01.95.F2") ||
      getenv("GSMLIB_FALCOM_A2_1_FIX") != NULL)
  {
    _capabilities._veryShortCOPSanswer = true;
  }

  // handle Motorola SMS store bug - wrong status code
  if ((info._manufacturer == "Motorola" &&
       info._model == "L Series"))
  {
    _capabilities._wrongSMSStatusCode = true;
  } 
 
  // handle Nokia Cellular Card Phone RPE-1 GSM900 and
  // Nokia Card Phone RPM-1 GSM900/1800 bug - CDS means CDSI
  if ((info._manufacturer == "Nokia Mobile Phones" &&
       (info._model == "Nokia Cellular Card Phone RPE-1 GSM900" ||
        info._model == "Nokia Card Phone RPM-1 GSM900/1800")))
  {
    _capabilities._CDSmeansCDSI = true;
  } 

  // find out whether we are supposed to send an acknowledgment
  Parser p(_at->chat("+CSMS?", "+CSMS:"));
  try {
    _capabilities._sendAck = p.parseInt() >= 1;
  }
  catch (GsmException &e)
  {
    if (e.getErrorClass() == ParserError) {
      _capabilities._sendAck = 0;
    } else {
      throw e;
    }
  }
      
  // set GSM default character set
  try
  {
    setCharSet("GSM");
  }
  catch (GsmException)
  {
    // ignore errors, some devices don't support this
  }

  // set default event handler
  // necessary to handle at least RING indications that might
  // otherwise confuse gsmlib
  _at->setEventHandler(&_defaultEventHandler);
}

MeTa::MeTa(Ref<Port> port) throw(GsmException) : _port(port)
{
  // initialize AT handling
  _at = new GsmAt(*this);

  init();
}

// MeTa::MeTa(Ref<GsmAt> at) throw(GsmException) :
//   _at(at)
// {
//   init();
// }

void MeTa::setPIN(std::string pin) throw(GsmException)
{
  _at->chat("+CPIN=\"" + pin + "\"");
}

std::string MeTa::getPINStatus() throw(GsmException)
{
  Parser p(_at->chat("+CPIN?", "+CPIN:"));
  return p.parseString();
}

void MeTa::setPhonebook(std::string phonebookName) throw(GsmException)
{
  if (phonebookName != _lastPhonebookName)
  {
    _at->chat("+CPBS=\"" + phonebookName + "\"");
    _lastPhonebookName = phonebookName;
  }
}

std::string MeTa::setSMSStore(std::string smsStore, int storeTypes, bool needResultCode)
  throw(GsmException)
{
  if (_capabilities._cpmsParamCount == -1)
  {
    // count the number of parameters for the CPMS AT sequences
    _capabilities._cpmsParamCount = 1;
    Parser p(_at->chat("+CPMS=?", "+CPMS:"));
    p.parseStringList();
    while (p.parseComma(true))
    {
      ++_capabilities._cpmsParamCount;
      p.parseStringList();
    }
  }

  // optimatization: only set current SMS store if different from last call
  // or the result code is needed
  if (needResultCode || _lastSMSStoreName != smsStore)
  {
    _lastSMSStoreName = smsStore;

    // build chat string
    std::string chatString = "+CPMS=\"" + smsStore + "\"";
    for (int i = 1; i < MIN(_capabilities._cpmsParamCount, storeTypes); ++i)
      chatString += ",\"" + smsStore + "\"";

    return _at->chat(chatString, "+CPMS:");
  }
  return "";
}

void MeTa::getSMSStore(std::string &readDeleteStore,
                       std::string &writeSendStore,
                       std::string &receiveStore) throw(GsmException)
{
  Parser p(_at->chat("+CPMS?", "+CPMS:"));
  writeSendStore = receiveStore = "";
  readDeleteStore = p.parseString();
  p.parseComma();
  p.parseInt();
  p.parseComma();
  p.parseInt();
  if (p.parseComma(true))
  {
    writeSendStore = p.parseString();
    p.parseComma();
    p.parseInt();
    p.parseComma();
    p.parseInt();
    if (p.parseComma(true))
    {
      receiveStore = p.parseString();
    }
  }
}

void MeTa::waitEvent(GsmTime timeout) throw(GsmException)
{
  if (_at->wait(timeout))
    _at->chat();                // send AT, wait for OK, handle events
}

// aux function for MeTa::getMEInfo()

static std::string stringVectorToString(const std::vector<std::string>& v,
					char separator = '\n')
{
  if (v.empty())
    return "";

  // concatenate string in vector as rows
  std::string result;
  for (std::vector<std::string>::const_iterator i = v.begin();;)
  {
    std::string s = *i;
    // remove leading and trailing "s
    if (s.length() > 0 && s[0] == '"')
      s.erase(s.begin());
    if (s.length() > 0 && s[s.length() - 1] == '"')
      s.erase(s.end() - 1);

    result += s;
    // don't add end line to last
    if ( ++i == v.end() || !separator)
      break;
    result += separator;
  }
  return result;
}

MEInfo MeTa::getMEInfo() throw(GsmException)
{
  MEInfo result;
  // some TAs just return OK and no info line
  // leave the info empty in this case
  // some TAs return multirows with info like address, firmware version
  result._manufacturer =
    stringVectorToString(_at->chatv("+CGMI", "+CGMI:", false));
  result._model = stringVectorToString(_at->chatv("+CGMM", "+CGMM:", false));
  result._revision =
    stringVectorToString(_at->chatv("+CGMR", "+CGMR:", false));
  result._serialNumber =
    stringVectorToString(_at->chatv("+CGSN", "+CGSN:", false),0);
  return result;
}

std::vector<std::string> MeTa::getSupportedCharSets() throw(GsmException)
{
  Parser p(_at->chat("+CSCS=?", "+CSCS:"));
  return p.parseStringList();
}
    
std::string MeTa::getCurrentCharSet() throw(GsmException)
{
  if (_lastCharSet == "")
  {
    Parser p(_at->chat("+CSCS?", "+CSCS:"));
    _lastCharSet = p.parseString();
  }
  return _lastCharSet;
}

void MeTa::setCharSet(std::string charSetName) throw(GsmException)
{
  _at->chat("+CSCS=\"" + charSetName + "\"");
  _lastCharSet = "";
}

std::string MeTa::getExtendedErrorReport() throw(GsmException)
{
  return _at->chat("+CEER", "+CEER:");
}

void MeTa::dial(std::string number) throw(GsmException)
{
  _at->chat("D" + number + ";");
}

void MeTa::answer() throw(GsmException)
{
  _at->chat("A");
}

void MeTa::hangup() throw(GsmException)
{
  _at->chat("H");

}

std::vector<OPInfo> MeTa::getAvailableOPInfo() throw(GsmException)
{
  std::vector<OPInfo> result;
  std::vector<std::string> responses = _at->chatv("+COPS=?", "+COPS:");

  // special treatment for Falcom A2-1, answer looks like
  //   responses.push_back("(1,29341),(3,29340)");
  if (_capabilities._veryShortCOPSanswer)
  {
    if (responses.size() == 1)
    {
      Parser p(responses[0]);
      while (p.parseChar('(', true))
      {
        OPInfo opi;
        opi._status = (OPStatus)p.parseInt();
        p.parseComma();
        opi._numericName = p.parseInt();
        p.parseChar(')');
        p.parseComma(true);
        result.push_back(opi);
      }
    }
  }
  else
    // some formats I have encountered...
    //responses.push_back("2,,,31017,,(0,1),(2)");
    //responses.push_back("(3,\"UK CELLNET\",\"CLNET\",\"23410\")," 
    //                    "(3,\"ONE2 ONE\",\"ONE2ONE\",\"23430\"),"
    //                    "(3,\"ORANGE\",\"ORANGE\",\"23433\")");
    //responses.push_back("(2,\"D1-TELEKOM\",,26201),"
    //                    "(3,\"D2  PRIVAT\",,26202),,(0,1,3,4),(0,2)");
    // some phones arbitrarily split the response into several lines
    //responses.push_back("(1,\"AMENA\",,\"21403\"),"
    //                    "(3,\"MOVISTAR\",,\"21407\"),");
    //responses.push_back("(3,\"E VODAFONE\",,\"21401\"),,(0,1),(2)");

    // GSM modems might return
    // 1. quadruplets of info enclosed in brackets separated by comma
    // 2. several lines of quadruplets of info enclosed in brackets
    // 3. several lines of quadruplets without brackets and additional
    //    info at EOL (e.g. Nokia 8290)
    for (std::vector<std::string>::iterator i = responses.begin();
         i != responses.end(); ++i)
    {
//       while (i->length() > 0 && ! isprint((*i)[i->length() - 1]))
//         i->erase(i->length() - 1, 1);

      bool expectClosingBracket = false;
      Parser p(*i);
      while (1)
      {
        OPInfo opi;
        expectClosingBracket = p.parseChar('(', true);
        int status = p.parseInt(true);
        opi._status = (status == NOT_SET ? UnknownOPStatus : (OPStatus)status);
        p.parseComma();
        opi._longName = p.parseString(true);
        p.parseComma();
        opi._shortName = p.parseString(true);
        p.parseComma();
        try
        {
          opi._numericName = p.parseInt(true);
        }
        catch (GsmException &e)
        {
          if (e.getErrorClass() == ParserError)
          {
            // the Ericsson GM12 GSM modem returns the numeric ID as string
	    std::string s = p.parseString();
            opi._numericName = checkNumber(s);
          }
          else
            throw e;
        }
        try
        {
          opi._numericName = p.parseComma();
          p.parseInt(true);
        }
        catch (GsmException &e)
        {
          if (e.getErrorClass() == ParserError)
          {
            /* okay */
          }
          else
            throw e;
        }
        if (expectClosingBracket) p.parseChar(')');
        result.push_back(opi);
        if (! p.parseComma(true)) break;
        // two commas ",," mean the list is finished
        if (p.getEol() == "" || p.parseComma(true)) break;
      }
      // without brackets, the ME/TA must use format 3.
      if (! expectClosingBracket) break;
    }
  return result;
}

OPInfo MeTa::getCurrentOPInfo() throw(GsmException)
{
  OPInfo result;

  // 1. This exception thing is necessary because not all ME/TA combinations
  // might support all the formats and then return "ERROR".
  // 2. Additionally some modems return "ERROR" for all "COPS=3,n" command
  // and report only one format with the "COPS?" command (e.g. Nokia 8290).

  // get long format
  try
  {
    try
    {
      _at->chat("+COPS=3,0");
    }
    catch (GsmException &e)
    {
      if (e.getErrorClass() != ChatError) throw;
    }
    Parser p(_at->chat("+COPS?", "+COPS:"));
    result._mode = (OPModes)p.parseInt();
    // some phones (e.g. Nokia Card Phone 2.0) just return "+COPS: 0"
    // if no network connection
    if (p.parseComma(true))
    {
      if (p.parseInt() == 0)
      {
        p.parseComma();
        result._longName = p.parseString();
      }
    }
  }
  catch (GsmException &e)
  {
    if (e.getErrorClass() != ChatError) throw;
  }

  // get short format
  try
  {
    try
    {
      _at->chat("+COPS=3,1");
    }
    catch (GsmException &e)
    {
      if (e.getErrorClass() != ChatError) throw;
    }
    Parser p(_at->chat("+COPS?", "+COPS:"));
    result._mode = (OPModes)p.parseInt();
    // some phones (e.g. Nokia Card Phone 2.0) just return "+COPS: 0"
    // if no network connection
    if (p.parseComma(true))
    {
      if (p.parseInt() == 1)
      {
        p.parseComma();
        result._shortName = p.parseString();
      }
    }
  }
  catch (GsmException &e)
  {
    if (e.getErrorClass() != ChatError) throw;
  }

  // get numeric format
  try
  {
    try
    {
      _at->chat("+COPS=3,2");
    }
    catch (GsmException &e)
    {
      if (e.getErrorClass() != ChatError) throw;
    }
    Parser p(_at->chat("+COPS?", "+COPS:"));
    result._mode = (OPModes)p.parseInt();
    // some phones (e.g. Nokia Card Phone 2.0) just return "+COPS: 0"
    // if no network connection
    if (p.parseComma(true))
    {
      if (p.parseInt() == 2)
      {
        p.parseComma();
        try
        {
          result._numericName = p.parseInt();
        }
        catch (GsmException &e)
        {
          if (e.getErrorClass() == ParserError)
          {
            // the Ericsson GM12 GSM modem returns the numeric ID as string
	    std::string s = p.parseString();
            result._numericName = checkNumber(s);
          }
          else
            throw e;
        }
      }
    }
  }
  catch (GsmException &e)
  {
    if (e.getErrorClass() != ChatError) throw;
  }
  return result;
}

void MeTa::setCurrentOPInfo(OPModes mode,
                            std::string longName,
                            std::string shortName,
                            int numericName) throw(GsmException)
{
  bool done = false;
  if (longName != "")
  {
    try
    {
      _at->chat("+COPS=" + intToStr((int)mode) + ",0,\"" + longName + "\"");
      done = true;
    }
    catch (GsmException &e)
    {
      if (e.getErrorClass() != ChatError) throw;
    }
  }
  if (shortName != "" && ! done)
  {
    try
    {
      _at->chat("+COPS=" + intToStr((int)mode) + ",1,\"" + shortName + "\"");
      done = true;
    }
    catch (GsmException &e)
    {
      if (e.getErrorClass() != ChatError) throw;
    }
  }
  if (numericName != NOT_SET && ! done)
  {
    try
    {
      _at->chat("+COPS=" + intToStr((int)mode) + ",2," +
                intToStr(numericName));
      done = true;
    }
    catch (GsmException &e)
    {
      if (e.getErrorClass() != ChatError) throw;
    }
  }
  if (! done)
    throw GsmException(_("unable to set operator"), OtherError);
}

std::vector<std::string> MeTa::getFacilityLockCapabilities() throw(GsmException)
{
  std::string locks = _at->chat("+CLCK=?", "+CLCK:");
  // some TA don't add '(' and ')' (Option FirstFone)
  if (locks.length() && locks[0] != '(')
  {
    locks.insert(locks.begin(),'(');
    locks += ')';
  }
  Parser p(locks);
  return p.parseStringList();
}

bool MeTa::getFacilityLockStatus(std::string facility, FacilityClass cl)
  throw(GsmException)
{
  // some TA return always multiline response with all classes
  // (Option FirstFone)
  // !!! errors handling is correct (responses.empty() true) ?
  std::vector<std::string> responses = 
    _at->chatv("+CLCK=\"" + facility + "\",2,," + intToStr((int)cl),"+CLCK:",true);
  for (std::vector<std::string>::iterator i = responses.begin();
       i != responses.end(); ++i)
  {
    Parser p(*i);
    int enabled = p.parseInt();

    // if the first time and there is no comma this 
    // return direct state of classes
    // else return all classes
    if (i == responses.begin())
    {
      if (!p.parseComma(true))
        return enabled == 1;
    }
    else
      p.parseComma();

    if ( p.parseInt() == (int)cl )
      return enabled == 1;
  }
  return false;

//  Parser p(_at->chat("+CLCK=\"" + facility + "\",2,," + intToStr((int)cl),
//                     "+CLCK:"));
//  return p.parseInt() == 1;
}

void MeTa::lockFacility(std::string facility, FacilityClass cl, std::string passwd)
  throw(GsmException)
{
  if (passwd == "")
    _at->chat("+CLCK=\"" + facility + "\",1,," + intToStr((int)cl));
  else
    _at->chat("+CLCK=\"" + facility + "\",1,\"" + passwd + "\","
              + intToStr((int)cl));
}

void MeTa::unlockFacility(std::string facility, FacilityClass cl, std::string passwd)
  throw(GsmException)
{
  if (passwd == "")
    _at->chat("+CLCK=\"" + facility + "\",0,," + intToStr((int)cl));
  else
    _at->chat("+CLCK=\"" + facility + "\",0,\"" + passwd + "\","
              + intToStr((int)cl));
}

std::vector<PWInfo> MeTa::getPasswords() throw(GsmException)
{
  std::vector<PWInfo> result;
  Parser p(_at->chat("+CPWD=?", "+CPWD:"));
  while (1)
  {
    PWInfo pwi;
    if (!p.parseChar('(', true)) break; // exit if no new tuple
    pwi._facility = p.parseString();
    p.parseComma();
    pwi._maxPasswdLen = p.parseInt();
    p.parseChar(')');
    p.parseComma(true);
    result.push_back(pwi);
  }
  return result;
}

void MeTa::setPassword(std::string facility, std::string oldPasswd, std::string newPasswd)
  throw(GsmException)
{
  _at->chat("+CPWD=\"" + facility + "\",\"" + oldPasswd + "\",\"" +
            newPasswd + "\"");
}

bool MeTa::getNetworkCLIP() throw(GsmException)
{
  Parser p(_at->chat("+CLIP?", "+CLIP:"));
  p.parseInt();                 // ignore result code presentation
  p.parseComma();
  return p.parseInt() == 1;
}

void MeTa::setCLIPPresentation(bool enable) throw(GsmException)
{
  if (enable)
    _at->chat("+CLIP=1");
  else
    _at->chat("+CLIP=0");
}

bool MeTa::getCLIPPresentation() throw(GsmException)
{
  Parser p(_at->chat("+CLIP?", "+CLIP:"));
  return p.parseInt() == 1;     // ignore rest of line
}

void MeTa::setCallForwarding(ForwardReason reason,
                             ForwardMode mode,
                             std::string number,
                             std::string subaddr,
                             FacilityClass cl,
                             int forwardTime) throw(GsmException)
{
  // FIXME subaddr is currently ignored
  if (forwardTime != NOT_SET && (forwardTime < 0 || forwardTime > 30))
    throw GsmException(_("call forward time must be in the range 0..30"),
                       ParameterError);
  
  int numberType;
  number = removeWhiteSpace(number);
  if (number.length() > 0 && number[0] == '+')
  {
    numberType = InternationalNumberFormat;
    number = number.substr(1);  // skip the '+' at the beginning
  }
  else
    numberType = UnknownNumberFormat;
  _at->chat("+CCFC=" + intToStr(reason) + "," +  intToStr(mode) + "," 
            "\"" + number + "\"," +
            (number.length() > 0 ? intToStr(numberType) : "") +
            "," +  intToStr(cl) +
                                // FIXME subaddr and type
            (forwardTime == NOT_SET ? "" :
             (",,," + intToStr(forwardTime))));
}
                           
void MeTa::getCallForwardInfo(ForwardReason reason,
                              ForwardInfo &voice,
                              ForwardInfo &fax,
                              ForwardInfo &data) throw(GsmException)
{
  // Initialize to some sensible values:
  voice._active = false;
  voice._cl = VoiceFacility;
  voice._time = -1;
  voice._reason = NoReason;
  data._active = false;
  data._cl = DataFacility;
  data._time = -1;
  data._reason = NoReason;
  fax._active = false;
  fax._cl = FaxFacility;
  fax._time = -1;
  fax._reason = NoReason;

  std::vector<std::string> responses =
    _at->chatv("+CCFC=" + intToStr(reason) + ",2", "+CCFC:");
  if (responses.size() == 1)
  {
    // only one line was returned. We have to ask for all three classes
    // (voice, data, fax) separately
    responses.clear();
    responses.push_back(_at->chat("+CCFC=" + intToStr(reason) +
                                  ",2,,,1", "+CCFC:"));
    responses.push_back(_at->chat("+CCFC=" + intToStr(reason) +
                                  ",2,,,2", "+CCFC:"));
    responses.push_back(_at->chat("+CCFC=" + intToStr(reason) +
                                  ",2,,,4", "+CCFC:"));
  }

  for (std::vector<std::string>::iterator i = responses.begin();
       i != responses.end(); ++i)
  {
    Parser p(*i);
    int status = p.parseInt();
    p.parseComma();
    FacilityClass cl = (FacilityClass)p.parseInt();
    std::string number;
    std::string subAddr;
    int forwardTime = NOT_SET;
      
    // parse number
    if (p.parseComma(true))
    {
      number = p.parseString();
      p.parseComma();
      unsigned int numberType = p.parseInt();
      if (numberType == InternationalNumberFormat) number = "+" + number;

      // parse subaddr
      if (p.parseComma(true))
      {
        // FIXME subaddr type not handled
        subAddr = p.parseString(true);
        p.parseComma();
        p.parseInt(true);
          
        // parse forwardTime
        if (p.parseComma(true))
        {
          forwardTime = p.parseInt();
        }
      }
    }
    switch (cl)
    {
    case VoiceFacility:
      voice._active = (status == 1);
      voice._cl = VoiceFacility;
      voice._number = number;
      voice._subAddr = subAddr;
      voice._time = forwardTime;
      voice._reason = reason;
      break;
    case DataFacility:
      data._active = (status == 1);
      data._cl = DataFacility;
      data._number = number;
      data._subAddr = subAddr;
      data._time = forwardTime;
      data._reason =  reason;
      break;
    case FaxFacility:
      fax._active = (status == 1);
      fax._cl = FaxFacility;
      fax._number = number;
      fax._subAddr = subAddr;
      fax._time = forwardTime;
      fax._reason = reason;
      break;
    }
  }
}

int MeTa::getBatteryChargeStatus() throw(GsmException)
{
  Parser p(_at->chat("+CBC", "+CBC:"));
  return p.parseInt();
}

int MeTa::getBatteryCharge() throw(GsmException)
{
  Parser p(_at->chat("+CBC", "+CBC:"));
  p.parseInt();
  p.parseComma();
  return p.parseInt();
}

int MeTa::getFunctionalityLevel() throw(GsmException)
{
  try {
    Parser p(_at->chat("+CFUN?", "+CFUN:"));
    // some phones return functionality level like "(2)"
    bool expectClosingParen = p.parseChar('(', true);
    int result = p.parseInt();
    if (expectClosingParen)
      p.parseChar(')');
    return result;
  }
  catch (GsmException &x)
  {
    if (x.getErrorClass() == ChatError)
    {
      throw GsmException(_("Functionality Level commands not supported by ME"),
			 MeTaCapabilityError);
    } else {
      throw;
    }
  }
}

void MeTa::setFunctionalityLevel(int level) throw(GsmException)
{
  try {
    Parser p(_at->chat("+CFUN="  + intToStr(level)));
  } catch (GsmException &x) {
    if (x.getErrorClass() == ChatError)
    {
      // If the command AT+CFUN commands really aren't supported by the ME,
      // then this will throw an appropriate exception for us.
      getFunctionalityLevel();
      // If the number was just out of range, we get here.
      throw GsmException(_("Requested Functionality Level out of range"),
			 ParameterError);
    }
    throw;
  }
}

int MeTa::getSignalStrength() throw(GsmException)
{
  Parser p(_at->chat("+CSQ", "+CSQ:"));
  return p.parseInt();
}

int MeTa::getBitErrorRate() throw(GsmException)
{
  Parser p(_at->chat("+CSQ", "+CSQ:"));
  p.parseInt();
  p.parseComma();
  return p.parseInt();
}

std::vector<std::string> MeTa::getPhoneBookStrings() throw(GsmException)
{
  Parser p(_at->chat("+CPBS=?", "+CPBS:"));
  return p.parseStringList();
}

PhonebookRef MeTa::getPhonebook(std::string phonebookString,
                                bool preload) throw(GsmException)
{
  for (PhonebookVector::iterator i = _phonebookCache.begin();
       i !=  _phonebookCache.end(); ++i)
  {
    if ((*i)->name() == phonebookString)
      return *i;
  }
  PhonebookRef newPb(new Phonebook(phonebookString, _at, *this, preload));
  _phonebookCache.push_back(newPb);
  return newPb;
}

std::string MeTa::getServiceCentreAddress() throw(GsmException)
{
  Parser p(_at->chat("+CSCA?", "+CSCA:"));
  return p.parseString();
}

void MeTa::setServiceCentreAddress(std::string sca) throw(GsmException)
{
  int type;
  sca = removeWhiteSpace(sca);
  if (sca.length() > 0 && sca[0] == '+')
  {
    type = InternationalNumberFormat;
    sca = sca.substr(1, sca.length() - 1);
  }
  else
    type = UnknownNumberFormat;
  Parser p(_at->chat("+CSCA=\"" + sca + "\"," + intToStr(type)));
}

std::vector<std::string> MeTa::getSMSStoreNames() throw(GsmException)
{
  Parser p(_at->chat("+CPMS=?", "+CPMS:"));
  // only return <mem1> values
  return p.parseStringList();
}

SMSStoreRef MeTa::getSMSStore(std::string storeName) throw(GsmException)
{
  for (SMSStoreVector::iterator i = _smsStoreCache.begin();
       i !=  _smsStoreCache.end(); ++i)
  {
    if ((*i)->name() == storeName)
      return *i;
  }
  SMSStoreRef newSs(new SMSStore(storeName, _at, *this));
  _smsStoreCache.push_back(newSs);
  return newSs;
}

void MeTa::sendSMS(Ref<SMSSubmitMessage> smsMessage) throw(GsmException)
{
  smsMessage->setAt(_at);
  smsMessage->send();
}

void MeTa::sendSMSs(Ref<SMSSubmitMessage> smsTemplate, std::string text,
                    bool oneSMS,
                    int concatenatedMessageId)
  throw(GsmException)
{
  assert(! smsTemplate.isnull());

  // compute maximum text length for normal SMSs and concatenated SMSs
  unsigned int maxTextLength, concMaxTextLength;
  switch (smsTemplate->dataCodingScheme().getAlphabet())
  {
  case DCS_DEFAULT_ALPHABET:
    maxTextLength = 160;
    concMaxTextLength = 152;
    break;
  case DCS_EIGHT_BIT_ALPHABET:
    maxTextLength = 140;
    concMaxTextLength = 134;
    break;
  case DCS_SIXTEEN_BIT_ALPHABET:
    maxTextLength = 70;
    concMaxTextLength = 67;
    break;
  default:
    throw GsmException(_("unsupported alphabet for SMS"),
                       ParameterError);
    break;
  }

  // simple case, only send one SMS
  if (oneSMS || text.length() <= maxTextLength)
  {
    if (text.length() > maxTextLength)
      throw GsmException(_("SMS text is larger than allowed"),
                         ParameterError);
    smsTemplate->setUserData(text);
    sendSMS(smsTemplate);
  }
  else                          // send multiple SMSs
  {
    if (concatenatedMessageId != -1)
      maxTextLength = concMaxTextLength;

    int numMessages = (text.length() + maxTextLength - 1) / maxTextLength;
    if (numMessages > 255)
      throw GsmException(_("not more than 255 concatenated SMSs allowed"),
                         ParameterError);
    unsigned char numMessage = 0;
    while (true)
    {
      if (concatenatedMessageId != -1)
      {
        unsigned char udhs[] = {0x00, 0x03, concatenatedMessageId,
                                numMessages, ++numMessage};
        UserDataHeader udh(std::string((char*)udhs, 5));
        smsTemplate->setUserDataHeader(udh);
      }
      smsTemplate->setUserData(text.substr(0, maxTextLength));
      sendSMS(smsTemplate);
      if (text.length() < maxTextLength)
        break;
      text.erase(0, maxTextLength);
    }
  }
}

void MeTa::setMessageService(int serviceLevel) throw(GsmException)
{
  std::string s;
  switch (serviceLevel)
  {
  case 0:
    s = "0";
    break;
  case 1:
    s = "1";
    break;
  default:
    throw GsmException(_("only serviceLevel 0 or 1 supported"),
                       ParameterError);
  }
  // some devices (eg. Origo 900) don't support service level setting
  _at->chat("+CSMS=" + s, "+CSMS:", true);
}

unsigned int MeTa::getMessageService() throw(GsmException)
{
  try {
    Parser p(_at->chat("+CSMS?", "+CSMS:"));
    return p.parseInt();
  }
  catch (GsmException &e)
  {
    if (e.getErrorClass() == ParserError) {
      return 0;
    } else {
      throw e;
    }
  }
}

void MeTa::getSMSRoutingToTA(bool &smsRouted,
                             bool &cbsRouted,
                             bool &statusReportsRouted) throw(GsmException)
{
  Parser p(_at->chat("+CNMI?", "+CNMI:"));
  p.parseInt();
  int smsMode = 0;
  int cbsMode = 0;
  int statMode = 0;
  //  int bufferMode;

  //  bufferMode = 0;
  if (p.parseComma(true))
  {
    smsMode = p.parseInt();
    if (p.parseComma(true))
    {
      cbsMode = p.parseInt();
      if (p.parseComma(true))
      {
        statMode = p.parseInt();
        /*if (p.parseComma(true))
        {
          bufferMode = p.parseInt();
	}*/
      }
    }
  }

  smsRouted = (smsMode == 2) || (smsMode == 3);
  cbsRouted = (cbsMode == 2) || (cbsMode == 3);
  statusReportsRouted = (statMode == 1);
}

void MeTa::setSMSRoutingToTA(bool enableSMS, bool enableCBS,
                             bool enableStatReport,
                             bool onlyReceptionIndication)
  throw(GsmException)
{
  bool smsModesSet = false;
  bool cbsModesSet = false;
  bool statModesSet = false;
  bool bufferModesSet = false;

  // find out capabilities
  Parser p(_at->chat("+CNMI=?", "+CNMI:"));
  std::vector<bool> modes = p.parseIntList();
  std::vector<bool> smsModes(1);
  std::vector<bool> cbsModes(1);
  std::vector<bool> statModes(1);
  std::vector<bool> bufferModes(1);
  if (p.parseComma(true))
  {
    smsModes = p.parseIntList();
    smsModesSet = true;
    if (p.parseComma(true))
    {
      cbsModes = p.parseIntList();
      cbsModesSet = true;
      if (p.parseComma(true))
      {
        statModes = p.parseIntList();
        statModesSet = true;
        if (p.parseComma(true))
        {
          bufferModes = p.parseIntList();
          bufferModesSet = true;
        }
      }
    }
  }

  // now set the mode vectors to the default if not set
  if (! smsModesSet) smsModes[0] = true;
  if (! cbsModesSet) cbsModes[0] = true;
  if (! statModesSet) statModes[0] = true;
  if (! bufferModesSet) bufferModes[0] = true;
  
  std::string chatString;
    
  // now try to set some optimal combination depending on
  // ME/TA's capabilities

  // handle modes
  if (isSet(modes, 2))
    chatString = "2";
  else if (isSet(modes, 1))
    chatString = "1";
  else if (isSet(modes, 0))
    chatString = "0";
  else if (isSet(modes, 3))
    chatString = "3";

  if (onlyReceptionIndication)
  {
    // handle sms mode
    if (enableSMS)
    {
      if (isSet(smsModes, 1))
        chatString += ",1";
      else 
        throw GsmException(_("cannot route SMS messages to TE"),
                           MeTaCapabilityError);
    }
    else
      chatString += ",0";
      
    // handle cbs mode
    if (enableCBS)
    {
      if (isSet(cbsModes, 1))
        chatString += ",1";
      else if (isSet(cbsModes, 2))
        chatString += ",2";
      else 
        throw GsmException(_("cannot route cell broadcast messages to TE"),
                           MeTaCapabilityError);
    }
    else
      chatString += ",0";

    // handle stat mode
    if (enableStatReport)
    {
      if (isSet(statModes, 2))
        chatString += ",2";
      else 
        throw GsmException(_("cannot route status reports messages to TE"),
                           MeTaCapabilityError);
    }
    else
      chatString += ",0";
  }
  else
  {
    // handle sms mode
    if (enableSMS)
    {
      if (isSet(smsModes, 2))
        chatString += ",2";
      else if (isSet(smsModes, 3))
        chatString += ",3";
      else 
        throw GsmException(_("cannot route SMS messages to TE"),
                           MeTaCapabilityError);
    }
    else
      chatString += ",0";
      
    // handle cbs mode
    if (enableCBS)
    {
      if (isSet(cbsModes, 2))
        chatString += ",2";
      else if (isSet(cbsModes, 3))
        chatString += ",3";
      else 
        throw GsmException(_("cannot route cell broadcast messages to TE"),
                           MeTaCapabilityError);
    }
    else
      chatString += ",0";

    // handle stat mode
    if (enableStatReport)
    {
      if (isSet(statModes, 1))
        chatString += ",1";
      else if (isSet(statModes, 2))
        chatString += ",2";
      else 
        throw GsmException(_("cannot route status report messages to TE"),
                           MeTaCapabilityError);
    }
    else
      chatString += ",0";
  }

  // handle buffer mode but only if it was reported by the +CNMI=? command
  // the Ericsson GM12 GSM modem does not like it otherwise
  if (bufferModesSet)
    {
      if (isSet(bufferModes, 1))
	chatString += ",1";
      else
	chatString += ",0";
    }

  _at->chat("+CNMI=" + chatString);
}

bool MeTa::getCallWaitingLockStatus(FacilityClass cl)
  throw(GsmException)
{
  // some TA return always multiline response with all classes
  // (Option FirstFone)
  // !!! errors handling is correct (responses.empty() true) ?
  std::vector<std::string> responses = 
    _at->chatv("+CCWA=0,2," + intToStr((int)cl),"+CCWA:",true);
  for (std::vector<std::string>::iterator i = responses.begin();
       i != responses.end(); ++i)
  {
    Parser p(*i);
    int enabled = p.parseInt();

    // if the first time and there is no comma this 
    // return direct state of classes
    // else return all classes
    if (i == responses.begin())
    {
      if (! p.parseComma(true))
        return enabled == 1;
    }
    else
      p.parseComma();

    if (p.parseInt() == (int)cl)
      return enabled == 1;
  }
  return false;

}
void MeTa::setCallWaitingLockStatus(FacilityClass cl, bool lock)
  throw(GsmException)
{
  if(lock)
    _at->chat("+CCWA=0,1," + intToStr((int)cl));
  else
    _at->chat("+CCWA=0,0," + intToStr((int)cl));
}

void MeTa::setCLIRPresentation(bool enable) throw(GsmException)
{
  if (enable)
    _at->chat("+CLIR=1");
  else
    _at->chat("+CLIR=0");
}

int MeTa::getCLIRPresentation() throw(GsmException)
{
  // 0:according to the subscription of the CLIR service
  // 1:CLIR invocation
  // 2:CLIR suppression
  Parser p(_at->chat("+CLIR?", "+CLIR:"));
  return p.parseInt();
}

