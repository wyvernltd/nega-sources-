-- [[ Wyvern Velocity Silent Aim Standalone Script ]] --
-- Optimized & tailored specifically for Velocity Executor (and compatible executors)
-- Includes __namecall redirection on ShootWeapon remote, Target Selection, FOV Circle, Wall Check, & Clean UI.

local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local HttpService = game:GetService("HttpService")
local CoreGui = game:GetService("CoreGui")

local LocalPlayer = Players.LocalPlayer
if not LocalPlayer then
    repeat task.wait() LocalPlayer = Players.LocalPlayer until LocalPlayer
end

-- [[ CONFIGURATION & STATE ]] --
local Settings = {
    Enabled = true,
    Wallbang = false,
    HitChance = 100,
    FOVSize = 150,
    ShowFOV = true,
    TargetPart = "Head", -- "Head", "UpperTorso", "LowerTorso", "Random"
    TeamCheck = true,
    ToggleKey = Enum.KeyCode.F6
}

local CachedTarget = nil
local TargetPartChosen = nil

-- [[ UTILITY & TARGETING FUNCTIONS ]] --
local function getTeam(player)
    return player:GetAttribute("Team") or (player.Team and player.Team.Name)
end

local function isEnemy(player)
    if player == LocalPlayer then return false end
    if not player.Character then return false end
    local hum = player.Character:FindFirstChildOfClass("Humanoid")
    if not hum or hum.Health <= 0 then return false end
    if player.Character:GetAttribute("Invincible") == true then return false end
    
    if Settings.TeamCheck then
        local myTeam = getTeam(LocalPlayer)
        local targetTeam = getTeam(player)
        if myTeam and targetTeam and myTeam == targetTeam then
            return false
        end
    end
    return true
end

local function canSee(origin, targetPos, ignoreChar)
    local rp = RaycastParams.new()
    rp.FilterType = Enum.RaycastFilterType.Exclude
    local ignoreList = {LocalPlayer.Character, ignoreChar, workspace.CurrentCamera}
    local debris = workspace:FindFirstChild("Debris")
    if debris then table.insert(ignoreList, debris) end
    rp.FilterDescendantsInstances = ignoreList
    rp.IgnoreWater = true
    
    local ray = workspace:Raycast(origin, targetPos - origin, rp)
    return ray == nil
end

local function getSilentAimTarget()
    if not Settings.Enabled then return nil, nil end
    local myChar = LocalPlayer.Character
    if not myChar then return nil, nil end
    local myRoot = myChar:FindFirstChild("HumanoidRootPart")
    if not myRoot then return nil, nil end
    
    local cam = workspace.CurrentCamera
    if not cam then return nil, nil end
    
    local bestPart = nil
    local bestDist = math.huge
    local screenCenter = cam.ViewportSize / 2

    for _, plr in ipairs(Players:GetPlayers()) do
        if isEnemy(plr) then
            local char = plr.Character
            if char then
                local targetBoneName = Settings.TargetPart
                if targetBoneName == "Random" then
                    local bones = {"Head", "UpperTorso", "LowerTorso"}
                    targetBoneName = bones[math.random(1, #bones)]
                end
                
                local part = char:FindFirstChild(targetBoneName) or char:FindFirstChild("Head") or char:FindFirstChild("HumanoidRootPart")
                local root = char:FindFirstChild("HumanoidRootPart")
                if part and root then
                    local screenPos, onScreen = cam:WorldToViewportPoint(part.Position)
                    if not onScreen and not Settings.Wallbang then continue end
                    
                    local distFromCenter = (Vector2.new(screenPos.X, screenPos.Y) - screenCenter).Magnitude
                    if distFromCenter > Settings.FOVSize then continue end
                    
                    if not Settings.Wallbang and not canSee(cam.CFrame.Position, part.Position, char) then
                        continue
                    end
                    
                    local worldDist = (myRoot.Position - root.Position).Magnitude
                    if distFromCenter < bestDist then
                        bestDist = distFromCenter
                        bestPart = part
                    end
                end
            end
        end
    end
    
    return bestPart
end

-- Update target on heartbeat
RunService.Heartbeat:Connect(function()
    CachedTarget = getSilentAimTarget()
end)

-- [[ FOV CIRCLE DRAWING ]] --
local FOVCircle = nil
pcall(function()
    if Drawing and Drawing.new then
        FOVCircle = Drawing.new("Circle")
        FOVCircle.Visible = Settings.ShowFOV
        FOVCircle.Thickness = 1.5
        FOVCircle.Color = Color3.fromRGB(44, 171, 255)
        FOVCircle.Transparency = 0.8
        FOVCircle.NumSides = 60
        FOVCircle.Filled = false
    end
end)

RunService.RenderStepped:Connect(function()
    if FOVCircle then
        local cam = workspace.CurrentCamera
        if cam then
            FOVCircle.Position = cam.ViewportSize / 2
            FOVCircle.Radius = Settings.FOVSize
            FOVCircle.Visible = Settings.Enabled and Settings.ShowFOV
        end
    end
end)

-- [[ __NAMECALL HOOK ON SHOOTWEAPON (VELOCITY SILENT AIM) ]] --
local hookInstalled = false
local function installVelocityHook()
    if hookInstalled then return true end
    local networkRemotes = ReplicatedStorage:FindFirstChild("NetworkRemotes")
    if not networkRemotes then return false end
    local inventory = networkRemotes:FindFirstChild("Inventory")
    if not inventory then return false end
    local shootRemote = inventory:FindFirstChild("ShootWeapon")
    if not shootRemote then return false end
    
    local mt = getrawmetatable(game)
    local oldNamecall = mt.__namecall
    setreadonly(mt, false)
    
    local shootRemoteExpectsString = true

    mt.__namecall = newcclosure(function(self, ...)
        if checkcaller() then return oldNamecall(self, ...) end

        local method = getnamecallmethod()
        if self == shootRemote and method == "FireServer" then
            local args = {...}
            local data = args[1]
            local wasString = false
            
            if type(data) == "string" and (string.find(data, "Bullets", 1, true) or string.sub(data, 1, 1) == "{") then
                local okDec, decoded = pcall(HttpService.JSONDecode, HttpService, data)
                if okDec and type(decoded) == "table" and type(decoded.Bullets) == "table" then
                    data = decoded
                    wasString = true
                end
            end

            if type(data) == "table" and type(data.Bullets) == "table" then
                local modified = false
                
                if Settings.Enabled and CachedTarget and CachedTarget.Parent then
                    local hitChance = tonumber(Settings.HitChance) or 100
                    if math.random(1, 100) <= hitChance then
                        local headPos = CachedTarget.Position
                        for _, bullet in ipairs(data.Bullets) do
                            if bullet.Origin then
                                local dir = (headPos - bullet.Origin).Unit
                                local dist = (headPos - bullet.Origin).Magnitude
                                local pCf = CFrame.new(bullet.Origin, headPos)
                                
                                bullet.Direction = dir
                                bullet.CFrame = pCf
                                bullet.Hits = {{
                                    Instance = CachedTarget,
                                    Position = headPos,
                                    Distance = dist,
                                    Normal = -dir,
                                    Material = "SmoothPlastic",
                                    Exit = false
                                }}
                                modified = true
                                break
                            end
                        end
                    end
                end

                local function safeSendShoot(targetData)
                    if shootRemoteExpectsString == true then
                        local ok, enc = pcall(HttpService.JSONEncode, HttpService, targetData)
                        if ok and enc then
                            local s, r = pcall(oldNamecall, self, enc)
                            if s then return r end
                            if not string.find(tostring(r), "expects a table") then
                                shootRemoteExpectsString = false
                            end
                        end
                    elseif shootRemoteExpectsString == false then
                        return oldNamecall(self, targetData)
                    end
                    
                    local success, result = pcall(oldNamecall, self, targetData)
                    if not success then
                        if string.find(tostring(result), "expects a string") then
                            shootRemoteExpectsString = true
                            local ok2, enc2 = pcall(HttpService.JSONEncode, HttpService, targetData)
                            if ok2 and enc2 then
                                local suc2, res2 = pcall(oldNamecall, self, enc2)
                                if suc2 then return res2 end
                            end
                        end
                        return oldNamecall(self, unpack(args))
                    else
                        shootRemoteExpectsString = false
                        return result
                    end
                end

                if wasString then
                    local okEnc, reEnc = pcall(HttpService.JSONEncode, HttpService, data)
                    if okEnc and reEnc then
                        return oldNamecall(self, reEnc)
                    end
                end
                
                if not modified and shootRemoteExpectsString ~= true and not wasString then
                    return oldNamecall(self, unpack(args))
                end
                
                return safeSendShoot(data)
            end
        end

        return oldNamecall(self, ...)
    end)
    
    setreadonly(mt, true)
    hookInstalled = true
    return true
end

task.spawn(function()
    local attempts = 0
    while not hookInstalled and attempts < 20 do
        if installVelocityHook() then break end
        attempts = attempts + 1
        task.wait(1)
    end
end)

-- [[ SIMPLE CLEAN UI FOR VELOCITY SILENT AIM ]] --
local function createUI()
    pcall(function()
        local old = CoreGui:FindFirstChild("VelocitySilentAimUI")
        if old then old:Destroy() end
    end)
    
    local ScreenGui = Instance.new("ScreenGui")
    ScreenGui.Name = "VelocitySilentAimUI"
    ScreenGui.ResetOnSpawn = false
    pcall(function() ScreenGui.Parent = CoreGui end)
    if not ScreenGui.Parent then ScreenGui.Parent = LocalPlayer:WaitForChild("PlayerGui") end
    
    local Frame = Instance.new("Frame")
    Frame.Size = UDim2.new(0, 260, 0, 310)
    Frame.Position = UDim2.new(0, 30, 0.5, -155)
    Frame.BackgroundColor3 = Color3.fromRGB(18, 20, 24)
    Frame.BorderSizePixel = 0
    Frame.Active = true
    Frame.Draggable = true
    Frame.Parent = ScreenGui
    
    local UICorner = Instance.new("UICorner", Frame)
    UICorner.CornerRadius = UDim.new(0, 8)
    
    local UIStroke = Instance.new("UIStroke", Frame)
    UIStroke.Color = Color3.fromRGB(44, 171, 255)
    UIStroke.Thickness = 1.5
    
    local Title = Instance.new("TextLabel")
    Title.Size = UDim2.new(1, 0, 0, 36)
    Title.BackgroundTransparency = 1
    Title.Text = "  Velocity Silent Aim"
    Title.TextColor3 = Color3.fromRGB(255, 255, 255)
    Title.Font = Enum.Font.GothamBold
    Title.TextSize = 16
    Title.TextXAlignment = Enum.TextXAlignment.Left
    Title.Parent = Frame
    
    local Container = Instance.new("ScrollingFrame")
    Container.Size = UDim2.new(1, -20, 1, -45)
    Container.Position = UDim2.new(0, 10, 0, 40)
    Container.BackgroundTransparency = 1
    Container.ScrollBarThickness = 3
    Container.Parent = Frame
    
    local UIList = Instance.new("UIListLayout", Container)
    UIList.SortOrder = Enum.SortOrder.LayoutOrder
    UIList.Padding = UDim.new(0, 8)
    
    local function addToggle(text, stateKey, cb)
        local btn = Instance.new("TextButton")
        btn.Size = UDim2.new(1, 0, 0, 32)
        btn.BackgroundColor3 = Color3.fromRGB(26, 28, 34)
        btn.Font = Enum.Font.Gotham
        btn.TextSize = 13
        btn.TextColor3 = Color3.fromRGB(220, 220, 220)
        btn.Text = "  " .. text .. ": " .. (Settings[stateKey] and "ON" or "OFF")
        btn.TextXAlignment = Enum.TextXAlignment.Left
        btn.Parent = Container
        Instance.new("UICorner", btn).CornerRadius = UDim.new(0, 6)
        
        btn.MouseButton1Click:Connect(function()
            Settings[stateKey] = not Settings[stateKey]
            btn.Text = "  " .. text .. ": " .. (Settings[stateKey] and "ON" or "OFF")
            if cb then cb(Settings[stateKey]) end
        end)
    end
    
    local function addSlider(text, stateKey, min, max)
        local lbl = Instance.new("TextLabel")
        lbl.Size = UDim2.new(1, 0, 0, 32)
        lbl.BackgroundColor3 = Color3.fromRGB(26, 28, 34)
        lbl.Font = Enum.Font.Gotham
        lbl.TextSize = 13
        lbl.TextColor3 = Color3.fromRGB(220, 220, 220)
        lbl.Text = "  " .. text .. ": " .. tostring(Settings[stateKey])
        lbl.TextXAlignment = Enum.TextXAlignment.Left
        lbl.Parent = Container
        Instance.new("UICorner", lbl).CornerRadius = UDim.new(0, 6)
        
        local btnMinus = Instance.new("TextButton", lbl)
        btnMinus.Size = UDim2.new(0, 26, 0, 26)
        btnMinus.Position = UDim2.new(1, -56, 0, 3)
        btnMinus.Text = "-"
        btnMinus.BackgroundColor3 = Color3.fromRGB(40, 44, 52)
        btnMinus.TextColor3 = Color3.fromRGB(255, 255, 255)
        Instance.new("UICorner", btnMinus).CornerRadius = UDim.new(0, 4)
        
        local btnPlus = Instance.new("TextButton", lbl)
        btnPlus.Size = UDim2.new(0, 26, 0, 26)
        btnPlus.Position = UDim2.new(1, -28, 0, 3)
        btnPlus.Text = "+"
        btnPlus.BackgroundColor3 = Color3.fromRGB(40, 44, 52)
        btnPlus.TextColor3 = Color3.fromRGB(255, 255, 255)
        Instance.new("UICorner", btnPlus).CornerRadius = UDim.new(0, 4)
        
        local step = (stateKey == "HitChance") and 5 or 25
        btnMinus.MouseButton1Click:Connect(function()
            Settings[stateKey] = math.clamp(Settings[stateKey] - step, min, max)
            lbl.Text = "  " .. text .. ": " .. tostring(Settings[stateKey])
        end)
        btnPlus.MouseButton1Click:Connect(function()
            Settings[stateKey] = math.clamp(Settings[stateKey] + step, min, max)
            lbl.Text = "  " .. text .. ": " .. tostring(Settings[stateKey])
        end)
    end
    
    local function addDropdown(text, stateKey, options)
        local btn = Instance.new("TextButton")
        btn.Size = UDim2.new(1, 0, 0, 32)
        btn.BackgroundColor3 = Color3.fromRGB(26, 28, 34)
        btn.Font = Enum.Font.Gotham
        btn.TextSize = 13
        btn.TextColor3 = Color3.fromRGB(220, 220, 220)
        btn.Text = "  " .. text .. ": " .. tostring(Settings[stateKey])
        btn.TextXAlignment = Enum.TextXAlignment.Left
        btn.Parent = Container
        Instance.new("UICorner", btn).CornerRadius = UDim.new(0, 6)
        
        local idx = 1
        for i, opt in ipairs(options) do
            if opt == Settings[stateKey] then idx = i break end
        end
        
        btn.MouseButton1Click:Connect(function()
            idx = (idx % #options) + 1
            Settings[stateKey] = options[idx]
            btn.Text = "  " .. text .. ": " .. tostring(Settings[stateKey])
        end)
    end
    
    addToggle("Enabled (F6)", "Enabled")
    addToggle("Wallbang", "Wallbang")
    addToggle("Show FOV Circle", "ShowFOV")
    addSlider("FOV Radius", "FOVSize", 25, 500)
    addSlider("Hit Chance %", "HitChance", 0, 100)
    addDropdown("Target Bone", "TargetPart", {"Head", "UpperTorso", "LowerTorso", "Random"})
    addToggle("Team Check", "TeamCheck")
    
    UserInputService.InputBegan:Connect(function(input, gp)
        if not gp and input.KeyCode == Settings.ToggleKey then
            Settings.Enabled = not Settings.Enabled
            -- Refresh UI text if visible
            for _, child in ipairs(Container:GetChildren()) do
                if child:IsA("TextButton") and string.find(child.Text, "Enabled") then
                    child.Text = "  Enabled (F6): " .. (Settings.Enabled and "ON" or "OFF")
                end
            end
        end
    end)
end

createUI()
print("[Wyvern] Velocity Silent Aim Standalone loaded successfully!")
