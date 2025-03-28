function example_3D_planning_with_pointcloud_PBVS()
    clear; clc; close all;

    %% ------------------------ Define Color Scheme ------------------------
    colors_struct = struct(...
        'PaleGrey1', '#EAF0F9', ...
        'LightBlueGrey', '#C7D6ED', ...
        'PaleGrey2', '#EAF9FE', ...
        'RobinsEggBlue', '#8DD6EF', ...
        'White', '#FFFFFF', ...
        'DuckEggBlue', '#B5E5D6', ...
        'OffWhite', '#F3F8E9', ...
        'Eggshell', '#CFE9C2', ...
        'VeryLightPink1', '#FFF6EE', ...
        'LightPeach', '#FFDDC5', ...
        'VeryLightPink2', '#FFECED', ...
        'PaleRose', '#FFB7B7', ...
        'LightPink', '#FFDBDF', ...
        'RosePink', '#F8A9A9', ...
        'Black', '#000000'...
    );

    %% --------------------- 1) Load and Downsample the Point Cloud -------
    ptCloud = pcread('vine_simple.pcd');
    collisionRadius = 0.01;  

    %% --------------------- 2) 3D Environment & Problem Setup ------------
    goals = [
        0.23958  0.27   0.38      0.43975;   % x-coords
        0.6      0.6    0.6       0.6;       % y-coords
        0.6098   0.6386 0.66107   0.65       % z-coords
    ];
    Delta_max = 0.1;

    %% --------------------- PLANNER Hyperparameters -----------------------
    dt        = 0.05;            
    N         = 5;              
    I         = 10;           
    lambda    = 0.5;            
    sigma_eps = (1e-2^2)*eye(3);
    nu        = 1.0;            
    R         = 0.1*eye(3);     
    Delta_max_pos = 0.05;     % Maximum allowed position change per step
    Delta_max_orient = 1; % Maximum allowed orientation change (in radians) per step

    %% --------------------- Model Definition ------------------------------
    model = struct();
    model.step = @(u, dt, x) x + u;   % Simple double-integrator style

    n_states  = 3; 
    n_actions = 3; 

    %% --------------------- Cost Functions --------------------------------
    stage_cost    = @(x,u,g) stageCostPointCloud(x, u, g, ptCloud, collisionRadius, Delta_max);
    terminal_cost = @(x,u,g) norm(x - g, 2)^2;

    %% --------------------- Create PLANNER Object -------------------------
    planner = PLANNER(model, N, I, dt, lambda, sigma_eps, nu, R, ...
                n_states, n_actions, @(x,u)0, @(x,u)0, Delta_max_pos, Delta_max_pos);

    %% --------------------- Initial State and Path ------------------------
    x = [0.0; 0.5; 0.6];  % start (for planning)

    %% --------------------- Figure / Axis Initialization ------------------
    fig_bg_color = hex2rgb(colors_struct.White);
    fig = figure('Name','3D PLANNER Waypoint Planning','Color', fig_bg_color);
    ax = axes('Parent', fig);
    hold(ax, 'on'); grid(ax, 'on');
    view(ax, 3);             
    axis(ax, 'equal');      
    xlabel(ax, 'X'); ylabel(ax, 'Y'); zlabel(ax, 'Z');
    set(ax, 'XLim', [-1 8], 'YLim', [-1 5], 'ZLim', [-1 5]);

    %% --------------------- 3) Show Point Cloud & Goals -------------------
    pcshow(ptCloud, 'Parent', ax, 'BackgroundColor','white');
    title(ax, 'Obstacle Point Cloud (Downsampled)', 'Color','k');

    goals_color = hex2rgb(colors_struct.RobinsEggBlue);
    plot3(ax, goals(1,:), goals(2,:), goals(3,:), 'o--',...
          'LineWidth', 2,'MarkerSize',8, 'Color', goals_color, ...
          'MarkerFaceColor', goals_color);

    for iGoal = 1:size(goals,2)
        text(goals(1,iGoal), goals(2,iGoal), goals(3,iGoal), ...
            sprintf('G%d', iGoal), 'Color','k',...
            'VerticalAlignment','bottom','HorizontalAlignment','left');
    end

    %% --------------------- Main Loop: Visit Goals in Sequence ------------
    camera_poses = [];  % Will store final path as [x, y, z, r, p, y]
    
    for goal_idx = 1:size(goals,2)
        current_goal = goals(:,goal_idx);
        disp(['Moving towards Goal ', num2str(goal_idx), ': (', ...
              num2str(current_goal(1)), ', ', num2str(current_goal(2)), ...
              ', ', num2str(current_goal(3)), ')']);
    
        while norm(x - current_goal) > 0.01
            planner.stage_cost    = @(x,u) stage_cost(x,u,current_goal);
            planner.terminal_cost = @(x,u) terminal_cost(x,u,current_goal);
    
            % Get new control from the planner
            U = planner.get_action(x);
            u_exec = U(:,1);
    
            % Step the "robot" in 3D position space
            x = model.step(u_exec, dt, x);
    
            % Sample camera orientations and select the best one
            best_orientation = sampleCameraOrientations(x, ptCloud, ax);
    
            % Store the current camera pose (position and orientation)
            camera_poses = [camera_poses; x', best_orientation]; %#ok<AGROW>
    
            % -------------- Visualization of Iteration Rollouts -----------
            cla(ax);
            hold(ax, 'on'); grid(ax, 'on');
            axis(ax, 'equal');
            set(ax, 'XLim', [-1 8], 'YLim', [-1 5], 'ZLim', [-1 5]);
            xlabel(ax, 'X'); ylabel(ax, 'Y'); zlabel(ax, 'Z');
            view(ax, 3);
    
            % Redraw point cloud
            pcshow(ptCloud, 'Parent', ax, 'BackgroundColor','white');
    
            % Redraw goals
            plot3(ax, goals(1,:), goals(2,:), goals(3,:), 'o--',...
                  'LineWidth',2,'MarkerSize',8, 'Color', goals_color, ...
                  'MarkerFaceColor', goals_color);
            for iG = 1:size(goals,2)
                text(goals(1,iG), goals(2,iG), goals(3,iG), ...
                    sprintf('G%d', iG), 'Color','k',...
                    'VerticalAlignment','bottom','HorizontalAlignment','left');
            end
    
            % Plot path so far
            path_color = hex2rgb(colors_struct.Black);
            plot3(ax, camera_poses(:,1), camera_poses(:,2), camera_poses(:,3), '.-', ...
                  'LineWidth',1.5, 'Color', path_color);
    
            % Current target
            plot3(ax, current_goal(1), current_goal(2), current_goal(3), ...
                  'o','MarkerSize',8, 'MarkerFaceColor', goals_color, 'MarkerEdgeColor','k');
    
            % Visualize camera frustum and visible points
            visualizeCameraFrustum(x, best_orientation, ptCloud, ax);
    
            drawnow;
        end
    end
    disp('Reached all goals!');

    %% ======================= POSITION-BASED VISUAL SERVOING =======================
    % We have 'camera_poses' as a sequence of [x, y, z, roll, pitch, yaw].
    % Instead of stopping at each waypoint, we'll define a "lookahead" approach.

    % 1) Convert 'camera_poses' to homogeneous transforms for easy manipulation.
    pathTransforms = cell(size(camera_poses,1), 1);
    for i = 1:size(camera_poses,1)
        p = camera_poses(i,1:3);
        eulAngles = camera_poses(i,4:6); % [roll, pitch, yaw]
        R = eul2rotm(eulAngles, 'XYZ');
        pathTransforms{i} = trvec2tform(p) * rotm2tform(R);
    end

    % 2) Load the Panda robot model
    robot = loadrobot("frankaEmikaPanda","DataFormat","row");
    endEffectorName = 'panda_hand';

    % --- Use IK to initialize the robot near the first transform in pathTransforms
    T_first = pathTransforms{1};
    % Optionally combine with an extra rotation about Y if you want the camera to point forward, etc.
    T_extraRot = eul2tform([0, pi/2, 0], "XYZ");
    T_start = T_first * T_extraRot;

    ik = inverseKinematics('RigidBodyTree', robot);
    weights = [1, 1, 1, 1, 1, 1];
    initialGuess = robot.homeConfiguration;
    [configSol, solInfo] = ik(endEffectorName, T_start, weights, initialGuess);
    q = configSol;  

    %% --------------------- PBVS Controller Parameters ----------------------
    % Gains for rotation and translation
    Kp_r = 5.0;   % rotational gain
    Kp_t = 40.0;   % translational gain
    % 'homogeneous_error' returns orientation in e(1:3), translation in e(4:6),
    % so we place rotation gains in the top-left corner:
    K = diag([Kp_r, Kp_r, Kp_r, Kp_t, Kp_t, Kp_t]);

    dt_servo  = 0.01;   % servo time step
    maxIter   = 50;     % maximum servo iterations
    threshold = 1e-2;   % final goal threshold

    % "Lookahead" offset in terms of path index.
    L = 1;  
    servoTrajectory = q;

    %% --------------------- Lookahead PBVS Loop ---------------------------
    velocityHistory = [];  
    refPositions = zeros(maxIter, 3);  % Store the position of the "lookahead" reference
    
    for iter = 1:maxIter
        
        % Current end-effector transform
        T_current = getTransform(robot, q, endEffectorName);
    
        % Check final pose threshold
        T_final = pathTransforms{end} * T_extraRot;
        err_final = homogeneous_error(T_final, T_current);
        if norm(err_final) < threshold
            disp('Lookahead PBVS: Reached final pose (within threshold).');
            % Trim refPositions if we exit early
            refPositions = refPositions(1:iter,:);
            break;
        end
    
        % Find the closest path index
        kClosest = findClosestPoseOnPath(T_current, pathTransforms);
        kLA = min(kClosest + L, length(pathTransforms));
        T_ref = pathTransforms{kLA} * T_extraRot;
    
        % Record the current reference position for plotting
        refPositions(iter,:) = T_ref(1:3,4)';
    
        % PBVS error & twist
        err_vec = homogeneous_error(T_ref, T_current);
        v = K * err_vec;   % 6x1 twist: [omega_x; omega_y; omega_z; v_x; v_y; v_z]
        velocityHistory = [velocityHistory, v];  %#ok<AGROW>
    
        % Jacobian-based IK step
        J = geometricJacobian(robot, q, endEffectorName);
        q_dot = pinv(J) * v;
        q = q + (q_dot' * dt_servo);
    
        % -------------- Add a disturbance at iteration = 30 (example) -----------
        if iter == 200
            disp('>>> Applying external disturbance to the robot joints! <<<');
            A small offset to all joints. Adjust magnitude or which joints to perturb as needed.
            q = q + 0.05*randn(size(q));  
            % or simply offset one joint, e.g. q(2) = q(2) + 0.1, etc.
        end
    
        servoTrajectory = [servoTrajectory; q]; %#ok<AGROW>
    end


    %% If the loop didn't break early, trim refPositions properly
    finalIter = size(servoTrajectory,1);  % actual number of servo steps
    if finalIter < size(refPositions,1)
        refPositions = refPositions(1:finalIter,:);
    end

    %% --------------------- Plot the Twist (v) ----------------------------
    nSamples = size(velocityHistory, 2);
    timeVec = (0:(nSamples-1)) * dt_servo;
    
    figure('Name','End-Effector Twist vs. Time');
    % Angular velocities in top subplot
    subplot(2,1,1);
    plot(timeVec, velocityHistory(1,:), 'LineWidth',1.5); hold on;
    plot(timeVec, velocityHistory(2,:), 'LineWidth',1.5);
    plot(timeVec, velocityHistory(3,:), 'LineWidth',1.5);
    legend('\omega_x','\omega_y','\omega_z','Location','Best');
    xlabel('Time [s]'); ylabel('Angular Velocity [rad/s]');
    title('End-Effector Angular Velocity (PBVS)');
    grid on;
    
    % Linear velocities in bottom subplot
    subplot(2,1,2);
    plot(timeVec, velocityHistory(4,:), 'LineWidth',1.5); hold on;
    plot(timeVec, velocityHistory(5,:), 'LineWidth',1.5);
    plot(timeVec, velocityHistory(6,:), 'LineWidth',1.5);
    legend('v_x','v_y','v_z','Location','Best');
    xlabel('Time [s]'); ylabel('Linear Velocity [m/s]');
    title('End-Effector Linear Velocity (PBVS)');
    grid on;

    %% =================== ANIMATE THE ROBOT MOTION (AND RECORD VIDEO) ===================
    fps = 60;
    writerObj = VideoWriter('panda_motion_PBVS_lookahead');
    writerObj.FrameRate = fps;
    open(writerObj);

    vidWidth  = 840;
    vidHeight = 630;
    figVideo = figure('Name','Recording Figure',...
        'Units','pixels','Position',[100 100 vidWidth vidHeight],...
        'Color','black',...
        'MenuBar','none',...
        'ToolBar','none',...
        'Resize','off',...
        'WindowStyle','normal');
    drawnow;  % force figure update

    axVideo = axes('Parent', figVideo, 'Units','pixels',...
                   'Position',[0 0 vidWidth vidHeight], 'Color','white');
    hold(axVideo, 'on');
    grid(axVideo, 'on');
    axis(axVideo,'equal');
    xlabel(axVideo, 'X'); ylabel(axVideo, 'Y'); zlabel(axVideo, 'Z');

    pcshow(ptCloud, 'Parent', axVideo, 'BackgroundColor','white');
    camlight('headlight');
    lighting gouraud;  
    material dull;
    view(axVideo, [20 45]);
    title(axVideo, 'Recording: Robot + Point Cloud + Path (Lookahead PBVS)','Color','k');

    % We'll also create an array of "pure" positions from pathTransforms for easy plotting
    pathPositions = zeros(length(pathTransforms),3);
    for iP = 1:length(pathTransforms)
        pathPositions(iP,:) = pathTransforms{iP}(1:3,4)';
    end

    % Animate
    numFrames = size(servoTrajectory,1);  % how many frames to animate
    for i = 1:numFrames
        cla(axVideo);

        % Plot the planned path (positions) and goals
        plot3(axVideo, pathPositions(:,1), pathPositions(:,2), pathPositions(:,3), ...
              '--','Color','red','LineWidth',1.5);
        plot3(axVideo, goals(1,:), goals(2,:), goals(3,:), 'o--',...
              'LineWidth',2, 'MarkerSize',8, 'Color','Red','MarkerFaceColor','black');
        for iGoal = 1:size(goals,2)
            text(goals(1,iGoal), goals(2,iGoal), goals(3,iGoal), ...
                 sprintf('G%d', iGoal), 'Color','White',...
                 'VerticalAlignment','top','HorizontalAlignment','left', ...
                 'Parent', axVideo);
        end

        pcshow(ptCloud, 'Parent', axVideo, 'BackgroundColor','black');
        camlight('headlight');
        lighting gouraud;  
        material dull;

        % Show robot in its current configuration
        show(robot, servoTrajectory(i,:), 'Parent', axVideo, ...
             'PreservePlot', false, 'FastUpdate', true,Visuals="off");
    
        % End-effector transform for camera frustum visualization
        tform_ee = getTransform(robot, servoTrajectory(i,:), endEffectorName);
        pos = tform_ee(1:3,4);
        eul_angles = rotm2eul(tform_ee(1:3,1:3)*eul2rotm([0,-pi/2,0], "XYZ"), 'XYZ');
        visualizeCameraFrustum(pos, eul_angles, ptCloud, axVideo);

        % Plot the *current lookahead reference* as a magenta point
        if i <= size(refPositions,1)
            refPos = refPositions(i,:);
            plot3(axVideo, refPos(1), refPos(2), refPos(3), 'mo', ...
                  'MarkerSize',10, 'LineWidth',1.5, 'MarkerFaceColor','m');
        end
    
        drawnow;
       
        % Capture the video frame
        frame = getframe(figVideo, [0 0 vidWidth vidHeight]);
        writeVideo(writerObj, frame);
    end

    close(writerObj);
    disp('Video recording complete.');
    
    %% =================== PLOT SERVO TRAJECTORY ===================
    % Extract end-effector positions from the stored servo joint configurations.
    numPoints = size(servoTrajectory, 1);
    ee_positions = zeros(numPoints, 3);
    for i = 1:numPoints
        T = getTransform(robot, servoTrajectory(i,:), endEffectorName);
        ee_positions(i,:) = T(1:3,4)';
    end
    
    figure;
    plot3(ee_positions(:,1), ee_positions(:,2), ee_positions(:,3), 'b.-', 'LineWidth', 1.5);
    hold on;
    plot3(camera_poses(:,1), camera_poses(:,2), camera_poses(:,3), 'r.--', 'LineWidth', 2);
    xlabel('X'); ylabel('Y'); zlabel('Z');
    legend('Servo Trajectory', 'Planned Path', 'Location', 'Best');
    title('End-Effector Servo Trajectory vs. Planned Path (Lookahead PBVS)');
    grid on;
    axis equal;
end

%% ================== HELPER FUNCTIONS ==================

function kClosest = findClosestPoseOnPath(H_current, pathTransforms)
    pCurrent = H_current(1:3,4);
    minDist = inf;
    kClosest = 1;
    for i = 1:length(pathTransforms)
        p_i = pathTransforms{i}(1:3,4);
        d = norm(p_i - pCurrent);
        if d < minDist
            minDist = d;
            kClosest = i;
        end
    end
end

% ---------------------- Stage Cost Function (Point Cloud) ---------------
function c = stageCostPointCloud(x, u, current_goal, ptCloud, collisionRadius, Delta_max)
    dist_goal = norm(x - current_goal, 2);
    
    [~, dists] = findNearestNeighbors(ptCloud, x', 1);
    nearestDist = dists(1);
    if nearestDist < collisionRadius
        penalty_obstacle = 1e2;
    else
        penalty_obstacle = 0;
    end

    % Soft constraint on maximum step distance
    % dt = 0.2;  
    % step_size = norm(u,2)*dt;
    % if step_size > Delta_max
    %     penalty_step = 1e3 * (step_size - Delta_max)^2;
    % else
        penalty_step = 0;
    % end

    % Control usage penalty
    penalty_control = 0.1 * (u.' * u);

    c = 100*dist_goal^2 + penalty_obstacle + penalty_step + penalty_control;
end

% ---------------------- Hex to RGB Helper -------------------------------
function rgb = hex2rgb(hexStr)
    hexStr = strrep(hexStr,'#','');
    if length(hexStr) ~= 6
        error('hex color code must be 6 characters, e.g. "#FF00FF"');
    end
    r = hex2dec(hexStr(1:2)) / 255;
    g = hex2dec(hexStr(3:4)) / 255;
    b = hex2dec(hexStr(5:6)) / 255;
    rgb = [r, g, b];
end

% ---------------------- Sample Camera Orientations ----------------------
function best_orientation = sampleCameraOrientations(position, ptCloud, ax)
    angle_increment = deg2rad(45);
    roll_angles = [0]; 
    pitch_angles = [0];
    yaw_angles = 0:angle_increment:(2*pi - angle_increment);

    max_visible = 0;
    best_orientation = [0, 0, 0];

    for roll = roll_angles
        for pitch = pitch_angles
            for yaw = yaw_angles
                orientation = [roll, pitch, yaw];
                visible_points = calculateVisiblePoints(position, orientation, ptCloud);
                if visible_points > max_visible
                    max_visible = visible_points;
                    best_orientation = orientation;
                end
            end
        end
    end

    visualizeCameraFrustum(position, best_orientation, ptCloud, ax);
end

% ---------------------- Calculate Visible Points ------------------------
function visible_points = calculateVisiblePoints(position, orientation, ptCloud)
    fov = 45; 
    max_range = 0.2; 
    min_range = 0.0; 

    Rwc = eul2rotm(orientation, 'XYZ'); 
    ptCloud_camera = transformPointCloud(ptCloud, position, Rwc);
    in_fov = isPointInFOV(ptCloud_camera, fov, max_range, min_range);
    visible_points = sum(in_fov);
end

% ---------------------- Visualize Camera Frustum -----------------------
function visualizeCameraFrustum(position, orientation, ptCloud, ax)
    fov = 60; 
    max_range = 0.2; 
    min_range = 0.0; 

    Rwc = eul2rotm(orientation, 'XYZ');
    ptCloud_camera = transformPointCloud(ptCloud, position, Rwc);

    in_fov = isPointInFOV(ptCloud_camera, fov, max_range, min_range);
    visible_points = ptCloud.Location(in_fov, :);

    plot3(ax, visible_points(:, 1), visible_points(:, 2), visible_points(:, 3), ...
          'g.', 'MarkerSize', 10);
    plot3(ax, position(1), position(2), position(3), 'ro', ...
          'MarkerSize', 10, 'MarkerFaceColor', 'r');

    plotFrustumEdges(position, orientation, fov, max_range, ax);
end

function ptCloud_camera = transformPointCloud(ptCloud, position, Rwc)
    points = ptCloud.Location;
    rCWw = position;
    points_camera = (Rwc.' * (points' - rCWw))';
    ptCloud_camera = pointCloud(points_camera);
end

function in_fov = isPointInFOV(ptCloud_camera, fov, max_range, min_range)
    points = ptCloud_camera.Location;
    x = points(:, 1);
    y = points(:, 2);
    z = points(:, 3);

    in_front = x > 0;
    angles_x = atan2d(y, x); 
    angles_y = atan2d(z, x); 

    in_fov_x = abs(angles_x) <= fov / 2; 
    in_fov_y = abs(angles_y) <= fov / 2; 
    in_range = (x >= min_range) & (x <= max_range);

    in_fov = in_front & in_fov_x & in_fov_y & in_range;
end

function plotFrustumEdges(position, orientation, fov, max_range, ax)
    fov_rad = deg2rad(fov);
    half_width = max_range * tan(fov_rad / 2);
    half_height = half_width;

    frustum_corners_camera = [
        0, 0, 0;
        max_range, -half_width, -half_height;
        max_range, -half_width,  half_height;
        max_range,  half_width,  half_height;
        max_range,  half_width, -half_height
    ];

    Rwc = eul2rotm(orientation, 'XYZ');
    frustum_corners_world = (Rwc * frustum_corners_camera')' + position';

    light_grey = [0.7, 0.7, 0.7];

    edges = [
        1 2; 1 3; 1 4; 1 5;
        2 3; 3 4; 4 5; 5 2
    ];
    for eIdx = 1:size(edges,1)
        idxA = edges(eIdx,1); idxB = edges(eIdx,2);
        plot3(ax, ...
            [frustum_corners_world(idxA,1), frustum_corners_world(idxB,1)], ...
            [frustum_corners_world(idxA,2), frustum_corners_world(idxB,2)], ...
            [frustum_corners_world(idxA,3), frustum_corners_world(idxB,3)], ...
            'Color', light_grey, 'LineWidth', 1.5);
    end
end

function e = homogeneous_error(H1, H2)
    e = zeros(6,1);

    R1 = H1(1:3, 1:3);
    R2 = H2(1:3, 1:3);
    Re = R1 * R2';
    t = trace(Re);
    eps_vec = [Re(3,2) - Re(2,3);
               Re(1,3) - Re(3,1);
               Re(2,1) - Re(1,2)];
    eps_norm = norm(eps_vec);
    
    if (t > -0.99 || eps_norm > 1e-10)
        if eps_norm < 1e-3
            orientation_error = (0.75 - t/12) * eps_vec;
        else
            orientation_error = (atan2(eps_norm, t - 1) / eps_norm) * eps_vec;
        end
    else
        orientation_error = (pi/2) * (diag(Re) + 1);
    end
    
    e(1:3) = orientation_error; 
    e(4:6) = H1(1:3,4) - H2(1:3,4);
end
