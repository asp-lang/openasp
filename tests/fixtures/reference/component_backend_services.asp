<%
Dim json
Dim parsed
Dim crypto
Dim http
Dim ai
Dim body
Dim port

port = Request("port")

Set json = Server.CreateObject("OpenASP.JSON")
Set parsed = json.Parse("{""name"":""OpenASP"",""items"":[1,true,null],""nested"":{""ok"":true},""emoji"":""\uD83D\uDE00"",""A"":1,""a"":2}")
Response.Write parsed("name")
Response.Write "|" & CStr(parsed("items")(0))
Response.Write "|" & CStr(parsed("items")(1))
Response.Write "|" & json.Stringify(parsed)
Response.Write "|" & CStr(json.Validate("{""valid"":true}"))
Response.Write "|" & json.Minify(" { ""x"" : 1 } ")
Response.Write "|" & CStr(json.Validate("{bad}"))
Response.Write "|" & CStr(LenB(parsed("emoji")))
Response.Write "|" & CStr(parsed("A")) & CStr(parsed("a"))

Set crypto = Server.CreateObject("OpenASP.Crypto")
Response.Write "|" & crypto.SHA256("abc")
Response.Write "|" & crypto.HMACSHA256("key", "The quick brown fox jumps over the lazy dog")
Response.Write "|" & crypto.Base64Encode("OpenASP")
Response.Write "|" & crypto.Base64Decode("T3BlbkFTUA==")
Response.Write "|" & crypto.Base64Encode(ChrB(0) & ChrB(255))
Response.Write "|" & CStr(crypto.ConstantTimeEquals("same", "same"))
Response.Write "|" & CStr(LenB(crypto.RandomBytes(16)))

Set http = Server.CreateObject("OpenASP.HttpClient")
http.Timeout = 5000
http.MaxResponseBytes = 1048576
Call http.SetHeader("X-OpenASP-Test", "http-client")
body = http.Post("http://127.0.0.1:" & port & "/echo", "{""hello"":""world""}")
Response.Write "|" & CStr(http.Status)
Response.Write "|" & body
Response.Write "|" & http.GetResponseHeader("X-Mock")

Set ai = Server.CreateObject("OpenASP.OpenAI")
ai.BaseURL = "http://127.0.0.1:" & port & "/v1"
ai.APIKey = "test-secret"
ai.Model = "mock-model"
body = ai.ChatCompletions("hello")
Response.Write "|" & CStr(ai.Status)
Response.Write "|" & body
body = ai.Responses("{""model"":""mock-model"",""input"":""raw""}")
Response.Write "|" & CStr(ai.Status)
Response.Write "|" & body
%>
