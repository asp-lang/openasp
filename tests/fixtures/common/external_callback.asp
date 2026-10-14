<%
Set math = Server.CreateObject("Acme.Math")
Set values = Server.CreateObject("Scripting.Dictionary")
values.Add "first", 1
values.Add "second", 2
math.Capture values
Response.Write math.CapturedCount
%>
