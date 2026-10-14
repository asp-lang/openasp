<%
Dim C_DATABASE
Const C_VIRT_DIR = ""
C_DATABASE = Server.MapPath(C_VIRT_DIR & "/db/QuickerSite.mdb")
Response.Write C_DATABASE
%>
