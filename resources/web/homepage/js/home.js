//var TestData={"sequence_id":"0","command":"get_recent_projects","response":[{"path":"D:\\work\\Models\\Toy\\3d-puzzle-cube-model_files\\3d-puzzle-cube.3mf","time":"2022\/3\/24 20:33:10"},{"path":"D:\\work\\Models\\Art\\Carved Stone Vase - remeshed+drainage\\Carved Stone Vase.3mf","time":"2022\/3\/24 17:11:51"},{"path":"D:\\work\\Models\\Art\\Kity & Cat\\Cat.3mf","time":"2022\/3\/24 17:07:55"},{"path":"D:\\work\\Models\\Toy\\鐩村墤.3mf","time":"2022\/3\/24 17:06:02"},{"path":"D:\\work\\Models\\Toy\\minimalistic-dual-tone-whistle-model_files\\minimalistic-dual-tone-whistle.3mf","time":"2022\/3\/22 21:12:22"},{"path":"D:\\work\\Models\\Toy\\spiral-city-model_files\\spiral-city.3mf","time":"2022\/3\/22 18:58:37"},{"path":"D:\\work\\Models\\Toy\\impossible-dovetail-puzzle-box-model_files\\impossible-dovetail-puzzle-box.3mf","time":"2022\/3\/22 20:08:40"}]};

var m_HotModelList=null;

function OnInit()
{
	//-----Official-----
    TranslatePage();
    let pluginTip = document.getElementById("NoPluginText");
    if (pluginTip)
        AppendTemplate(pluginTip, T("plugin_not_detected", "Bambu Network plug-in not detected. Click <a>here</a> to install it."),
                       pluginTip.querySelector("a"));

	SendMsg_GetLoginInfo();
	SendMsg_GetBambuLoginInfo();
	SendMsg_GetRecentFile();
	SendSimpleCommand("homepage_myprinters");
	setInterval(function(){ if($("#PrintersBoard").is(":visible")) SendSimpleCommand("homepage_myprinters"); }, 5000);
	SendMsg_GetStaffPick();

	Set_AccountMenu_Event();
}

//------最佳打开文件的右键菜单功能----------
var RightBtnFilePath='';

var MousePosX=0;
var MousePosY=0;
var sImages = {};
 
function Set_RecentFile_MouseRightBtn_Event()
{
	$(".FileItem").mousedown(
		function(e)
		{			
			//FilePath
			RightBtnFilePath=$(this).attr('fpath');
			
			if(e.which == 3){
				//鼠标点击了右键+$(this).attr('ff') );
				//ShowRecnetFileContextMenu();
			}else if(e.which == 2){
				//鼠标点击了中键
			}else if(e.which == 1){
				//鼠标点击了左键
				OnOpenRecentFile( encodeURI(RightBtnFilePath) );
			}
		});

	$(document).bind("contextmenu",function(e){
		//在这里书写代码，构建个性右键化菜单
		return false;
	});	
	
	// ORCA file actions
	$(".FileActions").mousedown(function(e){		
		return false; // Block events on empty area & parent
	});

	$(".FileActionsReveal").mousedown(function(e){		
		RightBtnFilePath=$(this).attr('fpath');
		if(e.which == 1 && RightBtnFilePath != "")
			OnExploreRecentFile();
		return false; // Block events from parent
	});
	$(".FileActionsRemove").mousedown(function(e){		
		RightBtnFilePath=$(this).attr('fpath');
		if(e.which == 1)
			OnDeleteRecentFile();
		return false; // Block events from parent
	});

    $(document).mousemove( function(e){
		MousePosX=e.pageX;
		MousePosY=e.pageY;
		
		//let ContextMenuWidth=$('#recnet_context_menu').width();
		//let ContextMenuHeight=$('#recnet_context_menu').height();
	
		let DocumentWidth=$(document).width();
		let DocumentHeight=$(document).height();
		
		//$("#DebugText").text( ContextMenuWidth+' - '+ContextMenuHeight+'<br/>'+
		//					 DocumentWidth+' - '+DocumentHeight+'<br/>'+
		//					 MousePosX+' - '+MousePosY +'<br/>' );
	} );
	

	$(document).click( function(){		
		var e = e || window.event;
        var elem = e.target || e.srcElement;
        while (elem) {
			//if (elem.id && elem.id == 'recnet_context_menu') {
            //        return;
			//}
			elem = elem.parentNode;
		}		
		
		//$("#recnet_context_menu").hide();
	} );

	
}

function SetLoginPanelVisibility(visible) {
  var leftBoard = document.getElementById("LeftBoard");
  leftBoard.style.display = "block";
}

function HandleStudio( pVal )
{
	let strCmd = pVal['command'];
	
	if (strCmd == "get_recent_projects") {
    ShowRecentFileList(pVal["response"]);
  } else if (strCmd == "orca_userlogin") {
    SetOrcaLoginInfo(pVal["data"]["avatar"], pVal["data"]["name"], pVal["data"]["account"]);
  } else if (strCmd == "orca_useroffline") {
    SetOrcaUserOffline();
  } else if (strCmd == "studio_bambu_userlogin") {
    SetBambuLoginInfo(pVal["data"]["avatar"], pVal["data"]["name"]);
  } else if (strCmd == "studio_bambu_useroffline") {
    SetBambuUserOffline();
  } else if (strCmd == "studio_set_mallurl") {
    SetMallUrl(pVal["data"]["url"]);
  } else if (strCmd == "studio_clickmenu") {
    let strName = pVal["data"]["menu"];

    GotoMenu(strName);
  } else if (strCmd == "cloud_providers_info") {
    var providers = (pVal["data"] && pVal["data"]["providers"]) || [];

    if (providers.indexOf("bbl") >= 0) {
      $("#BambuCloudSection").show();
    } else {
      SetBambuUserOffline();
      $("#BambuCloudSection").hide();
    }

    if (providers.indexOf("orca") >= 0) {
      $("#LeftBoard").show();
    } else {
      $("#LeftBoard").hide();
    }
  } else if (strCmd == "network_plugin_installtip") {
    // Bambu Cloud is unreachable without the network plugin, so its row only carries the tip.
    let bMissing = pVal["show"] * 1 == 1;

    $("#NoPluginTip").css("display", bMissing ? "block" : "none");
    $("#BambuAccount").toggleClass("Disabled", bMissing).attr("aria-disabled", bMissing ? "true" : null);
    if (bMissing) SetBambuSectionExpanded(true, false);
  } else if (strCmd == "modelmall_model_advise_get") {
    //alert('hot');
    if (m_HotModelList != null) {
      let SS1 = JSON.stringify(pVal["hits"]);
      let SS2 = JSON.stringify(m_HotModelList);

      if (SS1 == SS2) return;
    }

    m_HotModelList = pVal["hits"];
    ShowStaffPick(m_HotModelList);
  } else if (strCmd == "revo_mission_picker") {
    ShowMissionPicker(pVal["data"]);
  } else if (strCmd == "revo_mission") {
    ShowMissionHome(pVal["data"]);
  } else if (strCmd == "revo_printers") {
    ShowMyPrinters(pVal["printers"]);
  } else if (strCmd == "SetLoginPanelVisibility") {
    SetLoginPanelVisibility(pVal["data"]["visible"]);
  }
}

/*---- OrcaCloud opens as a native pane beside this page, so the shell only switches its
      own cloud-mode state and asks the app to show or hide that pane (WebViewPanel::set_cloud_mode). ----*/
var m_CloudMode=false;

function SetCloudMode( bOn )
{
	if( m_CloudMode==bOn )
		return;

	m_CloudMode=bOn;
	$("body").toggleClass("cloud-mode", bOn);

	if( bOn )
	{
		$(".BtnItem").removeClass("BtnItemSelected");
		$(".BtnItem[menu='orca_cloud']").addClass("BtnItemSelected");
	}
}

function GotoCloud()
{
	SetCloudMode(true);
	SendSimpleCommand("homepage_open_cloud");
}

function GotoMenu( strMenu )
{
	/*---- Any board in the right region means leaving the cloud pane. ----*/
	if( m_CloudMode )
	{
		SetCloudMode(false);
		SendSimpleCommand("homepage_close_cloud");
	}

	let MenuList=$(".BtnItem");
	let nAll=MenuList.length;
	
	for(let n=0;n<nAll;n++)
	{
		let OneBtn=MenuList[n];
		
		if( $(OneBtn).attr("menu")==strMenu )
		{
			$(".BtnItem").removeClass("BtnItemSelected");			
			
			$(OneBtn).addClass("BtnItemSelected");
			
			$("div[board]").hide();
			$("div[board=\'"+strMenu+"\']").show();
		}
	}
}

/*------Account rows------*/

/*----Everything that differs between the two cloud providers lives here----*/
var AccountRows = {
  orca:  { row: "#OrcaAccount",  name: "#UserName",      avatar: "#UserAvatarIcon",  dot: null,
           loginCmd: "homepage_orca_login_or_register",  logoutCmd: "homepage_orca_logout" },
  bambu: { row: "#BambuAccount", name: "#BambuUserName", avatar: "#BambuAvatarIcon", dot: "#BambuStatusDot",
           loginCmd: "homepage_bambu_login_or_register", logoutCmd: "homepage_bambu_logout" }
};

var BAMBU_FOLD_KEY = "OrcaHome_BambuCloudExpanded";

var m_OpenAccountMenu = null;

function SetAccountAvatar(selector, strAvatar) {
  if (strAvatar != null && strAvatar.trim() !== '') {
    if ($(selector).attr("src") !== strAvatar) $(selector).attr("src", strAvatar);
  } else {
    $(selector).removeAttr("src");
  }
}

function OnAvatarLoadError(img) {
  img.removeAttribute("src");  // fall back to the placeholder glyph
}

function SetAccountSignedIn(strProvider, strAvatar, strName, strHandle) {
  var account = AccountRows[strProvider];

  $(account.name).text(strName).attr("title", strName);  /*----long names are ellipsized----*/
  SetAccountAvatar(account.avatar, strAvatar);
  /*----the handle is the one thing the row does not already show----*/
  $(account.row).addClass("SignedIn").data("handle", strHandle !== strName ? strHandle : null);
  $(account.dot).addClass("Online");

  if (m_OpenAccountMenu === strProvider) OpenAccountMenu(strProvider);
}

function SetAccountSignedOut(strProvider) {
  var account = AccountRows[strProvider];

  if (m_OpenAccountMenu === strProvider) CloseAccountMenu();
  $(account.name).text('').removeAttr("title");
  SetAccountAvatar(account.avatar, null);
  $(account.row).removeClass("SignedIn").removeData("handle");
  $(account.dot).removeClass("Online");
}

function OnAccountRowClick(strProvider) {
  var account = AccountRows[strProvider];
  var row = $(account.row);
  if (row.hasClass("Disabled")) return;

  if (!row.hasClass("SignedIn")) {
    CloseAccountMenu();
    SendSimpleCommand(account.loginCmd);
  } else if (m_OpenAccountMenu === strProvider) {
    CloseAccountMenu();
  } else {
    OpenAccountMenu(strProvider);
  }
}

function OpenAccountMenu(strProvider) {
  var account = AccountRows[strProvider];
  var row = $(account.row);
  var top = row[0].offsetTop + row[0].offsetHeight + 6;  /*----read the geometry before writing----*/
  var strHandle = row.data("handle") || "";

  $("#AccountMenuHandle").text(strHandle).css("display", strHandle === "" ? "none" : "block");
  $("#AccountMenu").css("top", top + "px").addClass("Open");
  $(".AccountRow").removeClass("MenuOpen").attr("aria-expanded", "false");
  row.addClass("MenuOpen").attr("aria-expanded", "true");
  m_OpenAccountMenu = strProvider;
}

function CloseAccountMenu() {
  $("#AccountMenu").removeClass("Open");
  $(".AccountRow").removeClass("MenuOpen").attr("aria-expanded", "false");
  m_OpenAccountMenu = null;
}

function OnAccountMenuLogout() {
  var account = AccountRows[m_OpenAccountMenu];
  CloseAccountMenu();
  if (account) SendSimpleCommand(account.logoutCmd);
}

/*----Bambu Cloud is secondary, so its account folds away under the main one----*/
function SetBambuSectionExpanded(bExpanded, bPersist) {
  $("#BambuCloudBody").toggleClass("Expanded", bExpanded);
  $("#BambuAccount").attr("tabindex", bExpanded ? "0" : "-1");  /*----keep the folded row out of the tab order----*/
  $("#BambuCloudHeader").toggleClass("Expanded", bExpanded).attr("aria-expanded", bExpanded ? "true" : "false");
  if (!bExpanded && m_OpenAccountMenu === "bambu") CloseAccountMenu();
  // Best effort: the fold is a per-machine convenience, not a synced preference.
  if (bPersist) { try { localStorage.setItem(BAMBU_FOLD_KEY, bExpanded ? "1" : "0"); } catch (e) {} }
}

function ToggleBambuSection() {
  SetBambuSectionExpanded(!$("#BambuCloudBody").hasClass("Expanded"), true);
}

function Set_AccountMenu_Event() {
  var bExpanded = false;
  try { bExpanded = localStorage.getItem(BAMBU_FOLD_KEY) === "1"; } catch (e) {}
  SetBambuSectionExpanded(bExpanded, false);

  $(document).mousedown(function (e) {
    if (m_OpenAccountMenu === null) return;
    if ($(e.target).closest("#AccountMenu, .AccountRow").length === 0) CloseAccountMenu();
  });

  $(document).keydown(function (e) {
    if (m_OpenAccountMenu !== null && e.key === "Escape") CloseAccountMenu();
  });

  /*----Enter and Space activate the div-based buttons of this panel----*/
  $(document).on("keydown", "#LoginArea [role=button], #LoginArea [role=menuitem]", function (e) {
    if (e.key !== "Enter" && e.key !== " " && e.key !== "Spacebar") return;
    e.preventDefault();
    this.click();
  });
}

function SetMallUrl( strUrl )
{
	$("#MallWeb").prop("src",strUrl);
}


function ShowRecentFileList( pList )
{
	let nTotal=pList.length;
	
	let strHtml='';
	for(let n=0;n<nTotal;n++)
	{
		let OneFile=pList[n];
		
		let sPath=OneFile['path'];
		let sImg=OneFile["image"] || sImages[sPath];
		let sTime=OneFile['time'];
		let sName=OneFile['project_name'];
		let sPublished=OneFile['published'] == '1';
		sImages[sPath] = sImg;
		
		//let index=sPath.lastIndexOf('\\')>0?sPath.lastIndexOf('\\'):sPath.lastIndexOf('\/');
		//let sShortName=sPath.substring(index+1,sPath.length);

		let isExist  = !isNaN(sTime[0]); // its a valid file with time stamp
		let btnStyle = isExist ? "ButtonStyleConfirm" : "ButtonStyleDisabled";
		let btnPath  = isExist ? sPath : ""; // blank path will disable btn event
		
		let sBadge=sPublished? '<span class="FilePublishedBadge">'+T("published_badge", "PUB")+'</span>':'';
		let sLogoBadge=sPublished? '<img class="FileLogoBadge" src="../../images/OrcaSlicer_gradient_circle.svg" alt="" />':'';

		let TmpHtml='<div class="FileItem"  fpath="'+sPath+'"  >'+
						'<a class="FileTip" title="'+sPath+'"></a>'+
						'<div class="FileImg" ><img src="'+sImg+'" onerror="this.onerror=null;this.src=\'img/d.png\';"  alt="No Image"  />'+sLogoBadge+'</div>'+
						'<div class="FileNamePack">'+sBadge+'<div class="FileName TextS1">'+sName+'</div></div>'+
						'<div class="FileDate">'+sTime+'</div>'+
						'<div class="FileActions">'+
							'<div class="FileActionsReveal '+btnStyle+' ButtonTypeWindow trans" fpath="'+btnPath+'" data-i18n="show_in_folder">Show in folder</div>'+
							'<div class="FileActionsRemove ButtonStyleAlert ButtonTypeWindow" fpath="'+sPath+'">'+
								'<div class="icon16"/></div>'+
							'</div>'+
						'</div>'+
			    	'</div>';
		
		strHtml+=TmpHtml;
	}
	
	$("#FileList").html(strHtml);	
	TranslatePage();   // translate the data-i18n attributes on the freshly inserted cards
	
    Set_RecentFile_MouseRightBtn_Event();
	UpdateRecentClearBtnDisplay();
}
/*
function ShowRecnetFileContextMenu()
{
	$("#recnet_context_menu").offset({top: 10000, left:-10000});
	$('#recnet_context_menu').show();
	
	let ContextMenuWidth=$('#recnet_context_menu').width();
	let ContextMenuHeight=$('#recnet_context_menu').height();
	
    let DocumentWidth=$(document).width();
	let DocumentHeight=$(document).height();

	let RealX=MousePosX;
	let RealY=MousePosY;
	
	if( MousePosX + ContextMenuWidth + 24 >DocumentWidth )
		RealX=DocumentWidth-ContextMenuWidth-24;
	if( MousePosY+ContextMenuHeight+24>DocumentHeight )
		RealY=DocumentHeight-ContextMenuHeight-24;
	
	$("#recnet_context_menu").offset({top: RealY, left:RealX});
}
*/
/*-------RecentFile MX Message------*/
function SendMsg_GetLoginInfo()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="get_login_info";
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function SendSimpleCommand(command) {
  var tSend = {};
  tSend['sequence_id'] = Math.round(new Date() / 1000);
  tSend['command'] = command;
  SendWXMessage(JSON.stringify(tSend));
}

function SendMsg_GetOrcaLoginInfo() { SendSimpleCommand("get_orca_login_info"); }


function SendMsg_GetRecentFile()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="get_recent_projects";
	
	SendWXMessage( JSON.stringify(tSend) );
}


function OnClickModelDepot()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_modeldepot";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnClickNewProject()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_newproject";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnClickFindPrinters()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_findprinters";

	SendWXMessage( JSON.stringify(tSend) );
}

function OnClickOpenProject()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_openproject";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnOpenRecentFile( strPath )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_open_recentfile";
	tSend['data']={};
	tSend['data']['path']=decodeURI(strPath);
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function OnDeleteRecentFile( )
{
	//Clear in UI
	//$("#recnet_context_menu").hide();
	
	let AllFile=$(".FileItem");
	let nFile=AllFile.length;
	for(let p=0;p<nFile;p++)
	{
		let pp=AllFile[p].getAttribute("fpath");
		if(pp==RightBtnFilePath)
			$(AllFile[p]).remove();
	}	
	
	UpdateRecentClearBtnDisplay();
	
	//Send Msg to C++
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_delete_recentfile";
	tSend['data']={};
	tSend['data']['path']=RightBtnFilePath;
	
	SendWXMessage( JSON.stringify(tSend) );
}

function OnDeleteAllRecentFiles()
{
	$('#FileList').html('');
	UpdateRecentClearBtnDisplay();
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_delete_all_recentfile";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function UpdateRecentClearBtnDisplay()
{
    let AllFile=$(".FileItem");
	let nFile=AllFile.length;	
	if( nFile>0 )
		$("#RecentClearAllBtn").show();
	else
		$("#RecentClearAllBtn").hide();
}




function OnExploreRecentFile( )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_explore_recentfile";
	tSend['data']={};
	tSend['data']['path']=decodeURI(RightBtnFilePath);
	
	SendWXMessage( JSON.stringify(tSend) );	
	
	//$("#recnet_context_menu").hide();
}

// --- Cloud providers ---

function SetOrcaLoginInfo(strAvatar, strName, strAccount) { SetAccountSignedIn("orca", strAvatar, strName, strAccount); }
function SetOrcaUserOffline() { SetAccountSignedOut("orca"); }
function SetBambuLoginInfo(strAvatar, strName) { SetAccountSignedIn("bambu", strAvatar, strName, null); }
function SetBambuUserOffline() { SetAccountSignedOut("bambu"); }

function SendMsg_GetBambuLoginInfo() { SendSimpleCommand("get_bambu_login_info"); }

function BeginDownloadNetworkPlugin()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="begin_network_plugin_download";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OutputKey(keyCode, isCtrlDown, isShiftDown, isCmdDown) {
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "get_web_shortcut";
	tSend['key_event'] = {};
	tSend['key_event']['key'] = keyCode;
	tSend['key_event']['ctrl'] = isCtrlDown;
	tSend['key_event']['shift'] = isShiftDown;
	tSend['key_event']['cmd'] = isCmdDown;

	SendWXMessage(JSON.stringify(tSend));
}

//-------------User Manual------------

function OpenWikiUrl( strUrl )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="userguide_wiki_open";
	tSend['data']={};
	tSend['data']['url']=strUrl;
	
	SendWXMessage( JSON.stringify(tSend) );	
}

//--------------Staff Pick-------
var StaffPickSwiper=null;
function InitStaffPick()
{
	if( StaffPickSwiper!=null )
	{
		StaffPickSwiper.destroy(true,true);
		StaffPickSwiper=null;
	}	
	
	StaffPickSwiper = new Swiper('#HotModel_Swiper.swiper', {
            slidesPerView : 'auto',
		    spaceBetween: 16,
			navigation: {
				nextEl: '.swiper-button-next',
				prevEl: '.swiper-button-prev',
			},
		    slidesPerView : 'auto',
		    slidesPerGroup : 3

			});
}

function SendMsg_GetStaffPick()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="modelmall_model_advise_get";
	
	SendWXMessage( JSON.stringify(tSend) );
	
	setTimeout("SendMsg_GetStaffPick()",3600*1000*1);
}

function ShowStaffPick( ModelList )
{
	let PickTotal=ModelList.length;
	if(PickTotal==0)
	{
		$('#HotModelList').html('');
		$('#HotModelArea').hide();
		
		return;
	}
	
	let strPickHtml='';
	for(let a=0;a<PickTotal;a++)
	{
		let OnePickModel=ModelList[a];
		
		let ModelID=OnePickModel['design']['id'];
		let ModelName=OnePickModel['design']['title'];
		let ModelCover=OnePickModel['design']['cover']+'?image_process=resize,w_200/format,webp';
		
		let DesignerName=OnePickModel['design']['designCreator']['name'];
		let DesignerAvatar=OnePickModel['design']['designCreator']['avatar']+'?image_process=resize,w_32/format,webp';
		
		strPickHtml+='<div class="HotModelPiece swiper-slide"  onClick="OpenOneStaffPickModel('+ModelID+')" >'+
			    '<div class="HotModel_Designer_Info"><img src="'+DesignerAvatar+'" /><span class="TextS2">'+DesignerName+'</span></div>'+
				'	<div class="HotModel_PrevBlock"><img class="HotModel_PrevImg" src="'+ModelCover+'" /></div>'+
				'	<div  class="HotModel_NameText TextS1" title="'+ModelName+'">'+ModelName+'</div>'+
				'</div>';
	}
	
	$('#HotModelList').html(strPickHtml);
	InitStaffPick();
	$('#HotModelArea').show();
}

function OpenOneStaffPickModel( ModelID )
{
	//alert(ModelID);
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="modelmall_model_open";
	tSend['data']={};
	tSend['data']['id']=ModelID;
	
	SendWXMessage( JSON.stringify(tSend) );		
}


//---------------Global-----------------
/*---- [r]evo: My printers (start page) ----*/
function PbEsc(s){ return String(s==null?"":s).replace(/[&<>"']/g,function(c){return {"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c];}); }

function ShowMyPrinters(list)
{
	let html="";
	for(let i=0;i<list.length;i++)
	{
		let p=list[i];
		let cls="PbRow"+(p.active?" PbActive":"")+(p.online?"":" PbOffline");
		let col=!p.online?"":(p.kind=="gotowa"?"PbOk":(p.kind=="blad"?"PbErr":"PbBusy"));
		html+='<div class="'+cls+'" data-name="'+PbEsc(p.name)+'">'+
			'<img class="PbImg" src="../../profiles/Revotech/'+encodeURIComponent(p.cover)+'" onerror="this.style.visibility=\'hidden\'">'+
			'<div class="PbName"><div class="PbN">'+PbEsc(p.title)+'</div><div class="PbCapS">'+PbEsc(p.id||p.host)+'</div></div>'+
			'<div class="PbF PbW1"><div class="PbCapS">STATUS</div><div class="PbV '+col+'">'+(p.online?"● ":"○ ")+PbEsc(p.state)+'</div></div>'+
			'<div class="PbF PbW2"><div class="PbCapS">NOZZLE</div><div class="PbV">'+PbEsc(p.nozzle)+'</div></div>'+
			'<div class="PbF PbW3"><div class="PbCapS">BUILD</div><div class="PbV">'+PbEsc(p.dims)+'</div></div>'+
			'<div class="PbAct">'+(p.active?'<div class="PbOn">▶ ACTIVE</div>':'<div class="PbBtn">SELECT</div>')+'</div>'+
			'<div class="PbDel" title="Remove from My printers">✕</div>'+
			'</div>';
	}
	$("#PbList").html(html);
	let act=list.filter(function(p){return p.active;})[0];
	$("#CalibOn").text("NEW FILAMENT? START WITH THE GUIDED WIZARD"+(act?" · ON: "+act.title+" · "+act.nozzle:""));
	$("#PbEmpty").toggle(list.length==0);
	$("#PbList .PbDel").click(function(e){
		e.stopPropagation();
		let n=$(this).closest(".PbRow").attr("data-name");
		SendWXMessage(JSON.stringify({sequence_id:Math.round(new Date()/1000),command:"homepage_removeprinter",data:{name:n}}));
	});
	$("#PbList .PbRow").click(function(){
		let n=$(this).attr("data-name");
		if($(this).hasClass("PbActive")) return;
		SendWXMessage(JSON.stringify({sequence_id:Math.round(new Date()/1000),command:"homepage_selectprinter",data:{name:n}}));
	});
}

function RevoCalib(kind)
{
	SendWXMessage(JSON.stringify({sequence_id:Math.round(new Date()/1000),command:"homepage_calib",data:{kind:kind}}));
}

window.postMessage = HandleStudio;


/*---- [r]evo: New mission picker (belt printers) ----*/
var m_Mission = { mode: 0, gap: 50, seam: "auto", step: 20, endless: "0", copies: 1, eject: "off", bed_temp: 35 };

function MsChips(key, items, current, unit)
{
	let html="";
	let custom=true;
	for(let i=0;i<items.length;i++){
		let on=String(items[i][0])==String(current);
		if(on) custom=false;
		html+='<div class="MsChip'+(on?' PbSel':'')+'" data-v="'+items[i][0]+'">'+items[i][1]+'</div>';
	}
	if(unit) html+='<label class="MsCustom'+(custom?' PbSel':'')+'"><input aria-label="Custom value" value="'+(custom?current:'')+'" placeholder="..."><span>'+unit+'</span></label>';
	let box=$('.MsChips[data-key="'+key+'"]').html(html);
	box.find('.MsChip').click(function(){ m_Mission[key]=$(this).attr('data-v'); RenderMission(); });
	box.find('input').on('keydown',function(e){ if(e.key==='Enter') $(this).blur(); })
		.on('change',function(){ let v=parseFloat(String($(this).val()).replace(',','.')); if(v>0){ m_Mission[key]=v; RenderMission(); } });
}

function RenderMission()
{
	$('.PbMode').removeClass('PbSel');
	$('.PbMode[data-mode="'+m_Mission.mode+'"]').addClass('PbSel');
	let plates=m_Mission.mode==3;
	$('#MsGap').toggle(m_Mission.mode==1 || plates);
	$('#MsGapCap').text(plates ? 'GAP BETWEEN PLATES' : 'GAP BETWEEN OBJECTS');
	$('#MsCopiesCap').text(plates ? 'COPIES · HOW MANY PLATES' : 'COPIES · EACH OBJECT');
	if(!plates && m_Mission.copies*1<1) m_Mission.copies=1;
	MsChips('copies', plates ? [[2,'2'],[5,'5'],[7,'7'],[10,'10'],[0,'∞ UNTIL STOPPED']] : [[1,'1'],[10,'10'],[20,'20'],[50,'50']], m_Mission.copies, '×');
	$('#MsSeam').toggle(m_Mission.mode==2);
	MsChips('gap', [[30,'30'],[50,'50'],[80,'80'],[100,'100']], m_Mission.gap, 'MM');
	MsChips('seam', [['auto','AUTO'],['slope','SLOPE'],['steps','STEPS'],['vsteps','V-STEPS']], m_Mission.seam, null);
	$('#MsStep').toggle(m_Mission.seam=='steps' || m_Mission.seam=='vsteps');
	MsChips('step', [[10,'10'],[20,'20'],[30,'30']], m_Mission.step, 'MM');
	MsChips('endless', [['0','MODEL'],['1','ENDLESS']], m_Mission.endless, null);
	let vs=m_Mission.seam=='vsteps';
	$('#MsTeeth').toggle(vs);
	$('#MsSlope').toggle(m_Mission.seam=='slope');
	$('#MsRepeats').toggle(m_Mission.endless=='1');
	MsChips('tooth_width', [[4,'4'],[6,'6'],[8,'8'],[10,'10']], m_Mission.tooth_width, 'MM');
	MsChips('tooth_depth', [[5,'5'],[7,'7'],[9,'9'],[12,'12']], m_Mission.tooth_depth, 'MM');
	MsChips('tooth_shift', [[0,'0 · ALIGNED'],[0.25,'¼'],[0.33,'⅓'],[0.5,'½ · INTERLOCKED']], m_Mission.tooth_shift, null);
	MsChips('slope_angle', [[0,'AUTO'],[15,'15'],[20,'20'],[30,'30']], m_Mission.slope_angle, '°');
	MsChips('endless_count', [[0,'∞ UNTIL STOPPED'],[2,'2'],[5,'5'],[10,'10']], m_Mission.endless_count, '×');
	MsChips('segment', [[0,'MAX'],[50,'50'],[100,'100'],[150,'150']], m_Mission.segment, 'MM');
	MsChips('adv_speed', [[3,'3'],[5,'5'],[10,'10'],[30,'30']], m_Mission.adv_speed, 'MM/S');
	MsChips('lift', [[3,'3'],[5,'5'],[10,'10']], m_Mission.lift, 'MM');
	MsChips('clear_z', [], m_Mission.clear_z, 'MM');
	MsChips('clear_y', [], m_Mission.clear_y, 'MM');
	let warn='';
	if(m_Mission.seam=='steps' || vs){
		let free=m_Mission.step*1-(vs ? m_Mission.tooth_depth*1 : 0);
		if(free<m_Mission.clear_y*1) warn=(vs ? 'STEP LENGTH − TOOTH DEPTH' : 'STEP LENGTH')+' MUST BE LONGER THAN THE TOOLHEAD FRONT ('+m_Mission.clear_y+' MM)';
	}
	$('#MsWarn').text(warn);
	// Fixed bed: eject = "off", minutes (number) or "bed_temp"
	$('#MsEject').toggle(m_Mission.mode==0);
	MsChips('eject', [['off','OFF'],[5,'5 MIN'],[10,'10 MIN'],[20,'20 MIN'],['bed_temp','BED ≤ '+m_Mission.bed_temp+' °C']], m_Mission.eject, 'MIN');
}

function ShowMissionPicker(d)
{
	m_Mission = { mode: d.mode*1, gap: d.gap*1, seam: d.seam, step: d.step*1, endless: d.endless ? "1" : "0", copies: (d.copies==null ? 1 : d.copies*1),
		eject: d.eject=="time" ? d.eject_time*1 : (d.eject=="bed_temp" ? "bed_temp" : "off"), bed_temp: (d.eject_bed_temp==null ? 35 : d.eject_bed_temp*1),
		tooth_width: (d.tooth_width==null ? 8 : d.tooth_width*1), tooth_depth: (d.tooth_depth==null ? 9 : d.tooth_depth*1),
		tooth_shift: (d.tooth_shift==null ? 0.5 : Math.round(d.tooth_shift*100)/100), slope_angle: (d.slope_angle==null ? 0 : d.slope_angle*1),
		endless_count: (d.endless_count==null ? 0 : d.endless_count*1),
		segment: (d.segment==null ? 0 : d.segment*1), adv_speed: (d.adv_speed==null ? 30 : d.adv_speed*1), lift: (d.lift==null ? 5 : d.lift*1),
		clear_z: (d.clear_z==null ? 2.5 : d.clear_z*1), clear_y: (d.clear_y==null ? 11 : d.clear_y*1) };
	$('#MsVehicle').text(d.vehicle||'');
	$('.PbMode').off('click').click(function(){ m_Mission.mode=$(this).attr('data-mode')*1; RenderMission(); });
	RenderMission();
	$("div[board]").hide();
	$("#MissionBoard").show();
}

function StartMission()
{
	SendWXMessage(JSON.stringify({sequence_id:Math.round(new Date()/1000),command:"homepage_newmission",data:{
		mode:m_Mission.mode*1, gap:m_Mission.gap*1, seam:String(m_Mission.seam), step:m_Mission.step*1, endless:m_Mission.endless=="1",
		copies:Math.max(0, Math.round(m_Mission.copies*1)),
		eject:(m_Mission.eject=="off" || m_Mission.eject=="bed_temp") ? m_Mission.eject : "time",
		eject_time:(m_Mission.eject=="off" || m_Mission.eject=="bed_temp") ? 10 : Math.max(0, Math.round(m_Mission.eject*1)),
		tooth_width:m_Mission.tooth_width*1, tooth_depth:m_Mission.tooth_depth*1, tooth_shift:m_Mission.tooth_shift*1,
		slope_angle:m_Mission.slope_angle*1, segment:Math.max(0, m_Mission.segment*1), adv_speed:m_Mission.adv_speed*1, endless_count:Math.max(0, Math.round(m_Mission.endless_count*1)),
		lift:m_Mission.lift*1, clear_z:m_Mission.clear_z*1, clear_y:m_Mission.clear_y*1}}));
	GotoMenu('printers');
}


/*---- [r]evo Mission: start path and production strip ----*/
var m_Msn = { control: 0, remember: true, d: null };

function MsnSend(action, extra)
{
	SendWXMessage(JSON.stringify({sequence_id:Math.round(new Date()/1000),command:"homepage_mission",data:Object.assign({action:action},extra||{})}));
}
function MsnPick(c){ m_Msn.control=c; $("#MsnPath .MsnCard").each(function(){ $(this).toggleClass("on", $(this).attr("data-control")==String(c)); }); }
function MsnRemember(){ m_Msn.remember=!m_Msn.remember; $("#MsnBox").toggleClass("on", m_Msn.remember); }
function MsnStart(){ MsnSend("start", {control: !!m_Msn.control, remember: m_Msn.remember}); }

function ShowMissionHome(d)
{
	m_Msn.d=d;
	$("#MsnCap").text(d.control ? "[R]EVO MISSION CONTROL" : (d.ask ? "[R]EVO SLICER" : "[R]EVO SLICER · SIMPLE MISSION"));
	$("#MsnSwitch").toggle(!d.ask).text(d.control ? "SWITCH TO SIMPLE MISSION" : "SWITCH TO MISSION CONTROL ▸");
	if (d.ask && !$("#MsnPath").is(":visible")) MsnPick(d.control ? 1 : 0);
	$("#MsnPath").toggle(!!d.ask);
	let box=$("#MsnProd");
	if (!d.control || d.ask) { box.hide().empty(); return; }
	let s=d.summary;
	if (!s) {
		box.html('<div class="MsnNone">NO ORDERS YET · OPEN THE MISSION TAB TO ADD THE FIRST ONE</div>').show();
		box.find(".MsnNone").css("cursor","pointer").click(function(){ MsnSend("open",{view:"un"}); });
		return;
	}
	let n=s.nastepne;
	let tile=function(view,k,v,unit,detail){ return '<div class="MsnTile" data-view="'+view+'"><div class="k">'+k+'</div><div class="v">'+v+'<small>'+unit+'</small></div><div class="d">'+detail+'</div></div>'; };
	let html='<div class="PbSec" style="margin-top:22px"><span>PRODUCTION</span><a onClick="MsnSend(\'open\',{view:\'un\'})">OPEN MISSION ▸</a></div>';
	if (n) html+='<div class="MsnNext"><div class="k">NEXT</div><div class="t">'+PbEsc(n.numer)+' · '+PbEsc(n.nazwa)+' · '+n.dobre+' / '+n.sztuk+' PCS'+
		'<span>'+PbEsc(n.klient||'')+(n.drukarka?' · '+PbEsc(n.drukarka):'')+(n.termin?' · '+RTF('DUE %s',PbEsc(n.termin)):'')+'</span></div><div class="PbBtn PbBtnMain" onClick="MsnSend(\'open\',{view:\'pr\'})">PRINTERS</div></div>';
	let late=s.po_terminie?'<span class="r">'+s.po_terminie+' overdue</span> · ':'';
	html+='<div class="MsnStrip">'+
		tile('un','UNASSIGNED',s.nieprzydzielone,'ORDERS',s.nieprzydzielone?'<span class="a">waiting for a printer</span>':'all orders have a printer')+
		tile('as','ASSIGNED',s.przydzielone,'ORDERS',late+'<span class="a">'+s.w_tym_tygodniu+' due this week</span>')+
		tile('ok','COMPLETED',s.zakonczone,'ORDERS','<span class="g">ready to deliver and archive</span>')+
		tile('wy','UNASSIGNED PRINTS',s.wydruki_nieprzydzielone,'PRINTS',s.wydruki_nieprzydzielone?'<span class="a">attach to an order or client</span>':'every print is attached')+
		'</div>';
	box.html(html).show();
	box.find(".MsnTile").click(function(){ MsnSend("open",{view:$(this).attr("data-view")}); });
}
